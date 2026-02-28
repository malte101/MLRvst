/*
  ==============================================================================

    PluginProcessor.cpp
    mlrVST - Modern Edition Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PlayheadSpeedQuantizer.h"
#include "PresetStore.h"
#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr bool kEnableTriggerDebugLogging = false;

struct BarSelection
{
    int recordingBars = 1;
    float beatsPerLoop = 4.0f;
};

BarSelection decodeBarSelection(int value)
{
    switch (value)
    {
        case 25:  return { 1, 1.0f };   // 1/4 bar
        case 50:  return { 1, 2.0f };   // 1/2 bar
        case 100: return { 1, 4.0f };   // 1 bar
        case 200: return { 2, 8.0f };   // 2 bars
        case 400: return { 4, 16.0f };  // 4 bars
        case 800: return { 8, 32.0f };  // 8 bars
        // Backward compatibility (monome and legacy callers)
        case 1:   return { 1, 4.0f };
        case 2:   return { 2, 8.0f };
        case 4:   return { 4, 16.0f };
        case 8:   return { 8, 32.0f };
        default:  return { 1, 4.0f };
    }
}

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
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "stepedit";
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
    if (normalized == "stepedit" || normalized == "step_edit" || normalized == "step") { mode = MlrVSTAudioProcessor::ControlMode::StepEdit; return true; }
    return false;
}

juce::File getGlobalSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("GlobalSettings.xml");
}

constexpr int kStutterButtonFirstColumn = 9;
constexpr int kStutterButtonCount = 7;

uint8_t stutterButtonBitFromColumn(int column)
{
    if (column < kStutterButtonFirstColumn || column >= (kStutterButtonFirstColumn + kStutterButtonCount))
        return 0;
    return static_cast<uint8_t>(1u << static_cast<unsigned int>(column - kStutterButtonFirstColumn));
}

int countStutterBits(uint8_t mask)
{
    int count = 0;
    for (int i = 0; i < kStutterButtonCount; ++i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            ++count;
    }
    return count;
}

int highestStutterBit(uint8_t mask)
{
    for (int i = kStutterButtonCount - 1; i >= 0; --i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            return i;
    }
    return 0;
}

int lowestStutterBit(uint8_t mask)
{
    for (int i = 0; i < kStutterButtonCount; ++i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            return i;
    }
    return 0;
}

double stutterDivisionBeatsFromBit(int bit)
{
    static constexpr std::array<double, kStutterButtonCount> kDivisionBeats{
        1.0,            // bit 0 (col 9)  -> 1/4
        2.0 / 3.0,      // bit 1 (col 10) -> 1/4T
        0.5,            // bit 2 (col 11) -> 1/8
        1.0 / 3.0,      // bit 3 (col 12) -> 1/8T
        0.25,           // bit 4 (col 13) -> 1/16
        0.125,          // bit 5 (col 14) -> 1/32
        1.0 / 12.0      // bit 6 (col 15) -> 1/32T
    };
    const int idx = juce::jlimit(0, kStutterButtonCount - 1, bit);
    return kDivisionBeats[static_cast<size_t>(idx)];
}

double stutterDivisionBeatsFromBitForMacro(int bit, bool preferStraight)
{
    const double base = stutterDivisionBeatsFromBit(bit);
    if (!preferStraight)
        return base;

    switch (juce::jlimit(0, kStutterButtonCount - 1, bit))
    {
        case 1: return 0.5;   // 1/8 instead of 1/4T
        case 3: return 0.25;  // 1/16 instead of 1/8T
        case 6: return 0.125; // 1/32 instead of 1/32T
        default: return base;
    }
}

template <size_t N>
double snapDivisionToGrid(double divisionBeats, const std::array<double, N>& grid)
{
    if (!std::isfinite(divisionBeats))
        return grid[0];

    double best = grid[0];
    double bestDist = std::abs(divisionBeats - best);
    for (size_t i = 1; i < N; ++i)
    {
        const double cand = grid[i];
        const double dist = std::abs(divisionBeats - cand);
        if (dist < bestDist)
        {
            best = cand;
            bestDist = dist;
        }
    }
    return best;
}

double wrapUnitPhase(double phase)
{
    if (!std::isfinite(phase))
        return 0.0;
    phase = std::fmod(phase, 1.0);
    if (phase < 0.0)
        phase += 1.0;
    return phase;
}

float cutoffFromNormalized(float normalized)
{
    normalized = juce::jlimit(0.0f, 1.0f, normalized);
    return 20.0f * std::pow(1000.0f, normalized);
}

EnhancedAudioStrip::FilterAlgorithm filterAlgorithmFromIndex(int index)
{
    switch (juce::jlimit(0, 5, index))
    {
        case 0: return EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        case 1: return EnhancedAudioStrip::FilterAlgorithm::Tpt24;
        case 2: return EnhancedAudioStrip::FilterAlgorithm::Ladder12;
        case 3: return EnhancedAudioStrip::FilterAlgorithm::Ladder24;
        case 4: return EnhancedAudioStrip::FilterAlgorithm::MoogStilson;
        case 5:
        default: return EnhancedAudioStrip::FilterAlgorithm::MoogHuov;
    }
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
        if (supportsGrid())
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

void MonomeConnection::setArcRingMap(int encoder, const std::array<int, 64>& levels)
{
    if (!connected || !supportsArc())
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);

    juce::OSCMessage msg(oscPrefix + "/ring/map");
    msg.addInt32(clampedEncoder);
    for (int level : levels)
        msg.addInt32(juce::jlimit(0, 15, level));
    oscSender.send(msg);
}

void MonomeConnection::setArcRingLevel(int encoder, int ledIndex, int level)
{
    if (!connected || !supportsArc())
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);
    const int clampedLed = juce::jlimit(0, 63, ledIndex);
    const int clampedLevel = juce::jlimit(0, 15, level);
    oscSender.send(juce::OSCMessage(oscPrefix + "/ring/set", clampedEncoder, clampedLed, clampedLevel));
}

void MonomeConnection::setArcRingRange(int encoder, int start, int end, int level)
{
    if (!connected || !supportsArc())
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);
    const int clampedStart = juce::jlimit(0, 63, start);
    const int clampedEnd = juce::jlimit(0, 63, end);
    const int clampedLevel = juce::jlimit(0, 15, level);
    oscSender.send(juce::OSCMessage(oscPrefix + "/ring/range", clampedEncoder, clampedStart, clampedEnd, clampedLevel));
}

bool MonomeConnection::supportsGrid() const
{
    return !supportsArc();
}

bool MonomeConnection::supportsArc() const
{
    return currentDevice.type.containsIgnoreCase("arc");
}

int MonomeConnection::getArcEncoderCount() const
{
    if (!supportsArc())
        return 0;
    if (currentDevice.type.contains("2"))
        return 2;
    if (currentDevice.type.contains("4"))
        return 4;
    return 4;
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
        || address.startsWith(oscPrefix + "/tilt")
        || address.startsWith(oscPrefix + "/enc");
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
    else if (address.startsWith(oscPrefix + "/enc"))
        handleArcMessage(message);
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

void MonomeConnection::handleArcMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address == oscPrefix + "/enc/delta" && message.size() >= 2)
    {
        const int encoder = message[0].getInt32();
        const int delta = message[1].getInt32();
        if (onArcDelta)
            onArcDelta(encoder, delta);
    }
    else if (address == oscPrefix + "/enc/key" && message.size() >= 2)
    {
        const int encoder = message[0].getInt32();
        const int state = message[1].getInt32();
        if (onArcKey)
            onArcKey(encoder, state);
    }
}

//==============================================================================
// MlrVSTAudioProcessor Implementation
//==============================================================================

class MlrVSTAudioProcessor::PresetSaveJob final : public juce::ThreadPoolJob
{
public:
    PresetSaveJob(MlrVSTAudioProcessor& ownerIn, PresetSaveRequest requestIn)
        : juce::ThreadPoolJob("mlrVSTPresetSave_" + juce::String(requestIn.presetIndex + 1)),
          owner(ownerIn),
          request(std::move(requestIn))
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit())
        {
            owner.pushPresetSaveResult({ request.presetIndex, false });
            return jobHasFinished;
        }

        const bool success = owner.runPresetSaveRequest(request);
        owner.pushPresetSaveResult({ request.presetIndex, success });
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    PresetSaveRequest request;
};

MlrVSTAudioProcessor::MlrVSTAudioProcessor()
     : AudioProcessor(BusesProperties()
                      .withInput("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput("Strip 1", juce::AudioChannelSet::stereo(), true)
                      .withOutput("Strip 2", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 3", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 4", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 5", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 6", juce::AudioChannelSet::stereo(), false)),
       parameters(*this, nullptr, juce::Identifier("MlrVST"), createParameterLayout())
{
    // Initialize audio engine
    audioEngine = std::make_unique<ModernAudioEngine>();
    cacheParameterPointers();
    loadPersistentDefaultPaths();
    loadPersistentControlPages();
    setSwingDivisionSelection(swingDivisionSelection.load(std::memory_order_acquire));
    resetStepEditVelocityGestures();

    for (auto& held : arcKeyHeld)
        held = 0;
    for (auto& ring : arcRingCache)
        ring.fill(-1);
    arcControlMode = ArcControlMode::SelectedStrip;
    lastGridLedUpdateTimeMs = 0;
    
    // Setup monome callbacks
    monomeConnection.onKeyPress = [this](int x, int y, int state)
    {
        handleMonomeKeyPress(x, y, state);
    };
    monomeConnection.onArcDelta = [this](int encoder, int delta)
    {
        handleMonomeArcDelta(encoder, delta);
    };
    monomeConnection.onArcKey = [this](int encoder, int state)
    {
        handleMonomeArcKey(encoder, state);
    };
    
    monomeConnection.onDeviceConnected = [this]()
    {
        if (isTimerRunning())
            startTimer(monomeConnection.supportsArc() ? kArcRefreshMs : kGridRefreshMs);

        if (monomeConnection.supportsGrid())
        {
            // Force full LED resend after any reconnect to avoid stale cache mismatch.
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x)
                    ledCache[x][y] = -1;
        }

        for (auto& held : arcKeyHeld)
            held = 0;
        for (auto& ring : arcRingCache)
            ring.fill(-1);
        arcControlMode = ArcControlMode::SelectedStrip;
        arcSelectedModStep = 0;
        lastGridLedUpdateTimeMs = 0;

        // Defer LED update slightly to ensure everything is ready
        juce::MessageManager::callAsync([this]()
        {
            if (monomeConnection.supportsGrid())
                updateMonomeLEDs();
            if (monomeConnection.supportsArc())
                updateMonomeArcRings();
        });
    };

    monomeConnection.onDeviceDisconnected = [this]()
    {
        if (isTimerRunning())
            startTimer(kGridRefreshMs);
    };
    
    // Don't connect yet - wait for prepareToPlay
}

void MlrVSTAudioProcessor::cacheParameterPointers()
{
    masterVolumeParam = parameters.getRawParameterValue("masterVolume");
    limiterThresholdParam = parameters.getRawParameterValue("limiterThreshold");
    limiterEnabledParam = parameters.getRawParameterValue("limiterEnabled");
    quantizeParam = parameters.getRawParameterValue("quantize");
    innerLoopLengthParam = parameters.getRawParameterValue("innerLoopLength");
    grainQualityParam = parameters.getRawParameterValue("quality");
    pitchSmoothingParam = parameters.getRawParameterValue("pitchSmoothing");
    inputMonitorParam = parameters.getRawParameterValue("inputMonitor");
    crossfadeLengthParam = parameters.getRawParameterValue("crossfadeLength");
    triggerFadeInParam = parameters.getRawParameterValue("triggerFadeIn");
    outputRoutingParam = parameters.getRawParameterValue("outputRouting");
    pitchControlModeParam = parameters.getRawParameterValue("pitchControlMode");
    soundTouchEnabledParam = parameters.getRawParameterValue("soundTouchEnabled");

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
    presetSaveThreadPool.removeAllJobs(true, 4000);
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
        case ControlMode::StepEdit: return "Step Edit";
        case ControlMode::Normal:
        default: return "Normal";
    }
}

MlrVSTAudioProcessor::PitchControlMode MlrVSTAudioProcessor::getPitchControlMode() const
{
    const float rawChoice = (pitchControlModeParam != nullptr)
        ? pitchControlModeParam->load(std::memory_order_acquire)
        : 0.0f;
    const int modeIndex = juce::jlimit(0, 1, static_cast<int>(std::round(rawChoice)));
    return (modeIndex == 1) ? PitchControlMode::Resample : PitchControlMode::PitchShift;
}

void MlrVSTAudioProcessor::applyPitchControlToStrip(EnhancedAudioStrip& strip, float semitones)
{
    const float clampedSemitones = juce::jlimit(-24.0f, 24.0f, semitones);
    const float ratio = juce::jlimit(0.125f, 4.0f, std::pow(2.0f, clampedSemitones / 12.0f));
    const bool stripIsStepMode = (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step);

    if (stripIsStepMode)
    {
        // Step mode: keep control-domain semitone range unchanged, but expand
        // the resulting playback-speed range to +/-3 octaves (0.125x..8x).
        const float stepSpeedSemitones = clampedSemitones * 1.5f;
        const float stepRatio = juce::jlimit(0.125f, 8.0f, std::pow(2.0f, stepSpeedSemitones / 12.0f));
        strip.setPitchShift(clampedSemitones);
        if (auto* stepSampler = strip.getStepSampler())
            stepSampler->setSpeed(stepRatio);
        return;
    }

    if (getPitchControlMode() == PitchControlMode::Resample)
    {
        strip.setPitchShift(0.0f);
        strip.setPlaybackSpeed(ratio);
        return;
    }

    strip.setPlaybackSpeed(1.0f);
    strip.setPitchShift(clampedSemitones);
}

float MlrVSTAudioProcessor::getPitchSemitonesForDisplay(const EnhancedAudioStrip& strip) const
{
    if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
    {
        if (const auto* stepSampler = strip.getStepSampler())
            return juce::jlimit(-24.0f, 24.0f, static_cast<float>(stepSampler->getPitchOffset()) / 1.5f);
    }

    if (getPitchControlMode() == PitchControlMode::Resample)
    {
        const float ratio = juce::jmax(0.125f, strip.getPlaybackSpeed());
        const float semitones = 12.0f * std::log2(ratio);
        return juce::jlimit(-24.0f, 24.0f, semitones);
    }

    return strip.getPitchShift();
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
    const int maxDivision = static_cast<int>(EnhancedAudioStrip::SwingDivision::SixteenthTriplet);
    const int clamped = juce::jlimit(0, maxDivision, mode);
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

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "limiterThreshold",
        "Limiter Threshold (dB)",
        juce::NormalisableRange<float>(-24.0f, 0.0f, 0.1f),
        0.0f));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "limiterEnabled",
        "Limiter Enabled",
        false));
    
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quantize",
        "Quantize",
        juce::StringArray{"1", "1/2", "1/2T", "1/4", "1/4T", 
                          "1/8", "1/8T", "1/16", "1/16T", "1/32"},
        5));  // Default to 1/8

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "innerLoopLength",
        "Inner Loop Length",
        juce::StringArray{"1", "1/2", "1/4", "1/8", "1/16"},
        0));
    
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quality",
        "Grain Quality",
        juce::StringArray{"Linear", "Cubic", "Sinc", "Sinc HQ"},
        1));
    
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

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "triggerFadeIn",
        "Trigger Fade In (ms)",
        juce::NormalisableRange<float>(0.1f, 120.0f, 0.1f),
        12.0f));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "outputRouting",
        "Output Routing",
        juce::StringArray{"Stereo Mix", "Separate Strip Outs"},
        0));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "pitchControlMode",
        "Pitch Control Mode",
        juce::StringArray{"Pitch Shift", "Resample"},
        0));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "soundTouchEnabled",
        "SoundTouch Enabled",
        true));
    
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
            "Strip " + juce::String(i + 1) + " Playhead Speed",
            juce::NormalisableRange<float>(0.0f, 4.0f, 0.01f, 0.5f),
            1.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripPitch" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pitch",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f),
            0.0f));
    }
    
    return layout;
}

//==============================================================================
void MlrVSTAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    audioEngine->prepareToPlay(sampleRate, samplesPerBlock);
    lastAppliedSoundTouchEnabled = -1;
    lastGridLedUpdateTimeMs = 0;

    // Now safe to connect to monome
    if (!monomeConnection.isConnected())
        monomeConnection.connect(8000);

    // Clear all LEDs on startup
    juce::MessageManager::callAsync([this]()
    {
        if (monomeConnection.isConnected())
        {
            if (monomeConnection.supportsGrid())
            {
                monomeConnection.setAllLEDs(0);
                // Initialize LED cache
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 16; ++x)
                        ledCache[x][y] = -1;
            }
            if (monomeConnection.supportsArc())
            {
                for (auto& ring : arcRingCache)
                    ring.fill(-1);
                updateMonomeArcRings();
            }
        }
    });

    // Start LED update timer at 10fps (monome recommended refresh rate)
    if (!isTimerRunning())
        startTimer(kGridRefreshMs);
}

void MlrVSTAudioProcessor::releaseResources()
{
    stopTimer();
    monomeConnection.disconnect();
}

bool MlrVSTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Main output is fixed stereo; strip outputs are stereo buses.
    auto mainOutput = layouts.getMainOutputChannelSet();
    if (mainOutput != juce::AudioChannelSet::stereo())
        return false;

    // Aux outputs are either disabled or match main output channel set.
    const int outputBusCount = layouts.outputBuses.size();
    for (int bus = 1; bus < outputBusCount; ++bus)
    {
        const auto busSet = layouts.getChannelSet(false, bus);
        if (busSet != juce::AudioChannelSet::disabled() && busSet != mainOutput)
            return false;
    }

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

    if (limiterThresholdParam)
        audioEngine->setLimiterThresholdDb(limiterThresholdParam->load(std::memory_order_acquire));

    if (limiterEnabledParam)
        audioEngine->setLimiterEnabled(limiterEnabledParam->load(std::memory_order_acquire) > 0.5f);
    
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

    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);

    if (soundTouchEnabledParam)
    {
        const int enabledInt = (soundTouchEnabledParam->load(std::memory_order_acquire) > 0.5f) ? 1 : 0;
        if (enabledInt != lastAppliedSoundTouchEnabled)
        {
            audioEngine->setGlobalSoundTouchEnabled(enabledInt != 0);
            lastAppliedSoundTouchEnabled = enabledInt;
        }
    }

    // Apply any pending loop enter/exit actions that were quantized to timeline.
    applyPendingLoopChanges(posInfo);
    applyPendingBarChanges(posInfo);
    applyPendingStutterRelease(posInfo);
    applyPendingStutterStart(posInfo);

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
            {
                const float speedRatio = PlayheadSpeedQuantizer::quantizeRatio(
                    juce::jlimit(0.0f, 4.0f, speedParam->load(std::memory_order_acquire)));
                strip->setPlayheadSpeedRatio(speedRatio);
            }

            auto* pitchParam = stripPitchParams[static_cast<size_t>(i)];
            if (pitchParam)
                applyPitchControlToStrip(*strip, pitchParam->load(std::memory_order_acquire));
        }
    }

    applyMomentaryStutterMacro(posInfo);
    
    const bool separateStripRouting = (outputRoutingParam != nullptr && *outputRoutingParam > 0.5f);
    if (separateStripRouting && getBusCount(false) > 1)
    {
        std::array<std::array<float*, 2>, MaxStrips> stripBusChannels{};
        std::array<juce::AudioBuffer<float>, MaxStrips> stripBusViews;
        std::array<juce::AudioBuffer<float>*, MaxStrips> stripBusTargets{};
        stripBusTargets.fill(nullptr);

        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const int busIndex = stripIndex; // Strip 1 => main bus, others => aux buses.
            if (busIndex >= getBusCount(false))
                continue;

            auto busBuffer = getBusBuffer(buffer, false, busIndex);
            if (busBuffer.getNumChannels() <= 0 || busBuffer.getNumSamples() <= 0)
                continue;

            auto& channelPtrs = stripBusChannels[static_cast<size_t>(stripIndex)];
            channelPtrs.fill(nullptr);
            channelPtrs[0] = busBuffer.getWritePointer(0);
            channelPtrs[1] = (busBuffer.getNumChannels() > 1)
                                 ? busBuffer.getWritePointer(1)
                                 : busBuffer.getWritePointer(0);

            stripBusViews[static_cast<size_t>(stripIndex)].setDataToReferTo(
                channelPtrs.data(), 2, busBuffer.getNumSamples());
            stripBusTargets[static_cast<size_t>(stripIndex)] = &stripBusViews[static_cast<size_t>(stripIndex)];
        }

        // Keep playback robust if some aux buses are disabled in host: fallback to main bus.
        auto* mainTarget = stripBusTargets[0];
        if (mainTarget == nullptr)
            mainTarget = &buffer;
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            if (stripBusTargets[static_cast<size_t>(stripIndex)] == nullptr)
                stripBusTargets[static_cast<size_t>(stripIndex)] = mainTarget;
        }

        audioEngine->processBlock(buffer, midiMessages, posInfo, &stripBusTargets);
    }
    else
    {
        // Process audio
        audioEngine->processBlock(buffer, midiMessages, posInfo, nullptr);
    }
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
bool MlrVSTAudioProcessor::loadSampleToStrip(int stripIndex, const juce::File& file)
{
    if (file.existsAsFile() && stripIndex >= 0 && stripIndex < MaxStrips)
    {
        // Remember the folder for browsing context, but do NOT change
        // default XML paths here. Those are updated only by explicit
        // manual path selections (load button / Paths tab).
        lastSampleFolder = file.getParentDirectory();

        const bool loaded = audioEngine->loadSampleToStrip(stripIndex, file);
        if (loaded)
            currentStripFiles[stripIndex] = file;

        return loaded;
    }

    return false;
}

void MlrVSTAudioProcessor::captureRecentAudioToStrip(int stripIndex)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        const int bars = strip->getRecordingBars();
        audioEngine->captureLoopToStrip(stripIndex, bars);

        // Captured audio comes from the live input ring buffer, not a source file.
        // Clear stale path so preset save can embed the audio data.
        currentStripFiles[stripIndex] = juce::File();

        // Recording stop auto-trigger must still respect group choke behavior.
        audioEngine->triggerStripWithQuantization(stripIndex, 0, false);
        updateMonomeLEDs();
    }
}

void MlrVSTAudioProcessor::clearRecentAudioBuffer()
{
    if (!audioEngine)
        return;

    audioEngine->clearRecentInputBuffer();
}

void MlrVSTAudioProcessor::setPendingBarLengthApply(int stripIndex, bool pending)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    pendingBarLengthApply[static_cast<size_t>(stripIndex)] = pending;
}

bool MlrVSTAudioProcessor::canChangeBarLengthNow(int stripIndex) const
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return false;

    if (!strip->hasAudio() || !strip->isPlaying())
        return true;

    if (!strip->isPpqTimelineAnchored())
        return false;

    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (position->getPpqPosition().hasValue())
                return std::isfinite(*position->getPpqPosition());
        }
    }

    return false;
}

void MlrVSTAudioProcessor::requestBarLengthChange(int stripIndex, int bars)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const auto selection = decodeBarSelection(bars);
    setPendingBarLengthApply(stripIndex, false);

    if (!strip->hasAudio())
    {
        strip->setRecordingBars(selection.recordingBars);
        strip->setBeatsPerLoop(selection.beatsPerLoop);
        const juce::ScopedLock lock(pendingBarChangeLock);
        pendingBarChanges[static_cast<size_t>(stripIndex)].active = false;
        return;
    }

    if (!strip->isPlaying())
    {
        strip->setRecordingBars(selection.recordingBars);
        strip->setBeatsPerLoop(selection.beatsPerLoop);
        const juce::ScopedLock lock(pendingBarChangeLock);
        pendingBarChanges[static_cast<size_t>(stripIndex)].active = false;
        return;
    }

    const int quantizeDivision = getQuantizeDivision();
    // Bar changes are always PPQ-grid scheduled when host PPQ is valid.
    const bool useQuantize = (quantizeDivision >= 1);

    bool hasHostPpq = false;
    double currentPpq = std::numeric_limits<double>::quiet_NaN();
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (position->getPpqPosition().hasValue())
            {
                hasHostPpq = true;
                currentPpq = *position->getPpqPosition();
            }
        }
    }

    const bool syncReadyNow = hasHostPpq
        && std::isfinite(currentPpq)
        && strip->isPpqTimelineAnchored();

    const juce::ScopedLock lock(pendingBarChangeLock);
    auto& pending = pendingBarChanges[static_cast<size_t>(stripIndex)];
    pending.active = true;
    pending.recordingBars = selection.recordingBars;
    pending.beatsPerLoop = selection.beatsPerLoop;
    pending.quantized = useQuantize;
    pending.quantizeDivision = quantizeDivision;
    pending.targetPpq = std::numeric_limits<double>::quiet_NaN();

    // If PPQ/anchor is not currently valid, keep request pending and resolve the
    // target grid on the first PPQ-valid anchored audio block.
    if (!syncReadyNow)
        return;

    if (!pending.quantized)
        return;
    // Resolve quantized target on the audio thread to avoid GUI/playhead clock skew.
}

int MlrVSTAudioProcessor::getQuantizeDivision() const
{
    auto* quantizeParamLocal = parameters.getRawParameterValue("quantize");
    const int quantizeChoice = quantizeParamLocal ? static_cast<int>(*quantizeParamLocal) : 5;
    const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
    return (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
}

float MlrVSTAudioProcessor::getInnerLoopLengthFactor() const
{
    const int choice = innerLoopLengthParam
        ? juce::jlimit(0, 4, static_cast<int>(*innerLoopLengthParam))
        : 0;
    static constexpr std::array<float, 5> kFactors { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };
    return kFactors[static_cast<size_t>(choice)];
}

void MlrVSTAudioProcessor::queueLoopChange(int stripIndex, bool clearLoop, int startColumn, int endColumn, bool reverseDirection, int markerColumn)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const int quantizeDivision = getQuantizeDivision();
    // PPQ safety: clearing an active inner loop must always be grid-scheduled.
    const bool useQuantize = clearLoop || (quantizeEnabled && quantizeDivision > 1);

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
        // Strict PPQ safety: reject quantized loop changes until PPQ is valid.
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
    pending.quantizeDivision = quantizeDivision;
    pending.postClearTriggerArmed = false;
    pending.postClearTriggerColumn = 0;
}

void MlrVSTAudioProcessor::applyPendingLoopChanges(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();
    const double currentTempo = (posInfo.getBpm().hasValue() && *posInfo.getBpm() > 0.0)
        ? *posInfo.getBpm()
        : audioEngine->getCurrentTempo();

    std::array<PendingLoopChange, MaxStrips> readyChanges{};
    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (int i = 0; i < MaxStrips; ++i)
        {
            auto& pending = pendingLoopChanges[static_cast<size_t>(i)];
            if (!pending.active)
                continue;

            bool canApplyNow = false;
            if (!pending.quantized)
            {
                canApplyNow = std::isfinite(currentPpq);
            }
            else if (std::isfinite(currentPpq))
            {
                if (!std::isfinite(pending.targetPpq))
                {
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (targetPpq <= (currentPpq + 1.0e-6))
                        targetPpq += quantBeats;
                    pending.targetPpq = std::round(targetPpq / quantBeats) * quantBeats;
                    continue;
                }

                auto* strip = audioEngine->getStrip(i);
                const bool hasAnchor = (strip != nullptr) && strip->isPpqTimelineAnchored();
                const bool targetReached = (currentPpq + 1.0e-6 >= pending.targetPpq);
                if (targetReached && !hasAnchor)
                {
                    // Strict PPQ safety: never apply late/off-grid.
                    // If not anchor-safe at this grid, roll to the next grid.
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double nextTarget = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (nextTarget <= (currentPpq + 1.0e-6))
                        nextTarget += quantBeats;
                    pending.targetPpq = std::round(nextTarget / quantBeats) * quantBeats;
                    continue;
                }
                canApplyNow = hasAnchor && targetReached;
            }

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

        bool triggeredAtColumn = false;
        if (change.clear)
        {
            strip->clearLoop();
            strip->setReverse(false);
            if (change.markerColumn >= 0 && std::isfinite(currentPpq) && currentTempo > 0.0)
            {
                juce::AudioPlayHead::PositionInfo retriggerPosInfo;
                const double applyPpq = (change.quantized && std::isfinite(change.targetPpq))
                    ? change.targetPpq
                    : currentPpq;
                retriggerPosInfo.setPpqPosition(applyPpq);
                retriggerPosInfo.setBpm(currentTempo);
                strip->triggerAtSample(change.markerColumn, currentTempo, currentGlobalSample, retriggerPosInfo);
                triggeredAtColumn = true;
            }
            else if (change.markerColumn >= 0)
            {
                strip->setPlaybackMarkerColumn(change.markerColumn, currentGlobalSample);
            }
        }
        else
        {
            strip->setLoop(change.startColumn, change.endColumn);
            strip->setReverse(change.reverse);
        }

        if (change.quantized && !triggeredAtColumn)
        {
            // Deterministic PPQ realign after loop-geometry change.
            const double applyPpq = std::isfinite(currentPpq)
                ? currentPpq
                : (std::isfinite(change.targetPpq) ? change.targetPpq : audioEngine->getTimelineBeat());
            strip->realignToPpqAnchor(applyPpq, currentGlobalSample);
            strip->setBeatsPerLoopAtPpq(strip->getBeatsPerLoop(), applyPpq);
        }
        else
        {
            const bool markerApplied = (change.clear && change.markerColumn >= 0);
            if (!markerApplied && strip->isPlaying() && strip->hasAudio())
                strip->snapToTimeline(currentGlobalSample);
        }

        // Inner-loop clear gesture: the NEXT pad press while clear is pending
        // becomes the start column after exit, quantized like normal triggers.
        if (change.clear && change.postClearTriggerArmed)
        {
            const int targetColumn = juce::jlimit(0, MaxColumns - 1, change.postClearTriggerColumn);
            const int quantizeDivision = getQuantizeDivision();
            const bool useQuantize = quantizeEnabled && quantizeDivision > 1;
            audioEngine->triggerStripWithQuantization(i, targetColumn, useQuantize);
        }
    }
}

void MlrVSTAudioProcessor::applyPendingBarChanges(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine)
        return;

    if (!posInfo.getPpqPosition().hasValue())
        return;

    const double currentPpq = *posInfo.getPpqPosition();

    std::array<PendingBarChange, MaxStrips> readyChanges{};
    {
        const juce::ScopedLock lock(pendingBarChangeLock);
        for (int i = 0; i < MaxStrips; ++i)
        {
            auto& pending = pendingBarChanges[static_cast<size_t>(i)];
            if (!pending.active)
                continue;

            auto* strip = audioEngine->getStrip(i);
            const bool stripApplyReady = (strip != nullptr) && strip->hasAudio() && strip->isPlaying();
            const bool anchorReady = stripApplyReady && strip->isPpqTimelineAnchored();

            if (pending.quantized && !std::isfinite(pending.targetPpq))
            {
                if (!std::isfinite(currentPpq) || !anchorReady)
                    continue;

                const int division = juce::jmax(1, pending.quantizeDivision);
                const double quantBeats = 4.0 / static_cast<double>(division);
                double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
                if (targetPpq <= (currentPpq + 1.0e-6))
                    targetPpq += quantBeats;
                pending.targetPpq = std::round(targetPpq / quantBeats) * quantBeats;
                continue;
            }

            bool canApplyNow = false;
            if (!pending.quantized)
            {
                canApplyNow = std::isfinite(currentPpq)
                    && stripApplyReady
                    && strip->isPpqTimelineAnchored();
            }
            else if (std::isfinite(currentPpq) && std::isfinite(pending.targetPpq))
            {
                const bool hasAnchor = stripApplyReady && strip->isPpqTimelineAnchored();
                const bool targetReached = (currentPpq + 1.0e-6 >= pending.targetPpq);

                if (targetReached && !hasAnchor)
                {
                    // Keep the request alive and roll to the next grid if this
                    // strip is not anchor-safe on the current grid.
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double nextTarget = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (nextTarget <= (currentPpq + 1.0e-6))
                        nextTarget += quantBeats;
                    pending.targetPpq = std::round(nextTarget / quantBeats) * quantBeats;
                    continue;
                }

                canApplyNow = hasAnchor && targetReached;
            }

            if (!canApplyNow)
                continue;

            readyChanges[static_cast<size_t>(i)] = pending;
            pending.active = false;
        }
    }

    double currentTempo = audioEngine->getCurrentTempo();
    if (posInfo.getBpm().hasValue() && *posInfo.getBpm() > 0.0)
        currentTempo = *posInfo.getBpm();

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto& change = readyChanges[static_cast<size_t>(i)];
        if (!change.active)
            continue;

        auto* strip = audioEngine->getStrip(i);
        if (!strip || !strip->hasAudio() || !strip->isPlaying())
            continue;

        const double applyPpq = (change.quantized && std::isfinite(change.targetPpq))
            ? change.targetPpq
            : currentPpq;
        strip->setRecordingBars(change.recordingBars);
        strip->setBeatsPerLoopAtPpq(change.beatsPerLoop, applyPpq);
        if (std::isfinite(applyPpq) && currentTempo > 0.0)
        {
            // Match the preset-restore path so bar remaps re-anchor deterministically.
            strip->restorePresetPpqState(true,
                                         true,
                                         strip->getPpqTimelineOffsetBeats(),
                                         strip->getCurrentColumn(),
                                         currentTempo,
                                         applyPpq,
                                         currentGlobalSample);
        }
        // After target-grid remap, force a hard lock to the *current* host PPQ
        // so trigger/fallback references are consistent within this audio block.
        strip->realignToPpqAnchor(currentPpq, currentGlobalSample);
    }
}

void MlrVSTAudioProcessor::applyPendingStutterStart(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine || pendingStutterStartActive.load(std::memory_order_acquire) == 0)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();

    double targetPpq = pendingStutterStartPpq.load(std::memory_order_acquire);
    const int64_t currentSample = audioEngine->getGlobalSampleCount();

    // Match inner-loop quantized scheduling:
    // resolve target grid on audio thread to avoid GUI/playhead clock skew.
    if (!(std::isfinite(targetPpq) && targetPpq >= 0.0))
    {
        if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
            return;

        const double division = juce::jlimit(
            0.03125, 4.0, pendingStutterStartDivisionBeats.load(std::memory_order_acquire));
        targetPpq = std::ceil(currentPpq / division) * division;
        if (targetPpq <= (currentPpq + 1.0e-6))
            targetPpq += division;
        targetPpq = std::round(targetPpq / division) * division;
        pendingStutterStartPpq.store(targetPpq, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        return;
    }

    if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
        return;

    if (currentPpq + 1.0e-6 < targetPpq)
        return;

    double applyPpq = targetPpq;

    bool hasAnyPlayingStrip = false;
    bool anchorsReady = true;
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip || !strip->hasAudio() || !strip->isPlaying())
            continue;
        hasAnyPlayingStrip = true;
        if (!strip->isPpqTimelineAnchored())
        {
            anchorsReady = false;
            break;
        }
    }

    // Mirror inner-loop quantized-apply safety: if anchor isn't valid on this grid,
    // roll to the next stutter grid instead of entering off-sync.
    if (hasAnyPlayingStrip && !anchorsReady
        && std::isfinite(currentPpq)
        && std::isfinite(targetPpq))
    {
        const double division = juce::jlimit(
            0.03125, 4.0, pendingStutterStartDivisionBeats.load(std::memory_order_acquire));
        double nextTarget = std::ceil(currentPpq / division) * division;
        if (nextTarget <= (currentPpq + 1.0e-6))
            nextTarget += division;
        nextTarget = std::round(nextTarget / division) * division;
        pendingStutterStartPpq.store(nextTarget, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        return;
    }

    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);

    if (!std::isfinite(applyPpq))
        applyPpq = audioEngine->getTimelineBeat();
    performMomentaryStutterStartNow(applyPpq, currentSample);
}

void MlrVSTAudioProcessor::applyPendingStutterRelease(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine || pendingStutterReleaseActive.load(std::memory_order_acquire) == 0)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();

    const double targetPpq = pendingStutterReleasePpq.load(std::memory_order_acquire);
    const int64_t currentSample = audioEngine->getGlobalSampleCount();
    const int64_t targetSample = pendingStutterReleaseSampleTarget.load(std::memory_order_acquire);

    bool releaseReady = false;
    double applyPpq = currentPpq;

    // Primary path: PPQ-locked release.
    if (std::isfinite(targetPpq) && std::isfinite(currentPpq))
    {
        releaseReady = (currentPpq + 1.0e-6 >= targetPpq);
        applyPpq = targetPpq;
    }
    // Fallback path: sample-target release if PPQ is unavailable.
    else if (targetSample >= 0)
    {
        releaseReady = (currentSample >= targetSample);
    }
    // Safety fallback: never stay latched forever when host is not playing.
    else if (!posInfo.getIsPlaying())
    {
        releaseReady = true;
    }

    if (!releaseReady)
        return;

    pendingStutterReleaseActive.store(0, std::memory_order_release);
    pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);

    if (!std::isfinite(applyPpq))
        applyPpq = audioEngine->getTimelineBeat();
    performMomentaryStutterReleaseNow(applyPpq, currentSample);
}

void MlrVSTAudioProcessor::captureMomentaryStutterMacroBaseline()
{
    if (!audioEngine)
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        auto& saved = momentaryStutterSavedState[idx];
        saved = MomentaryStutterSavedStripState{};

        auto* strip = audioEngine->getStrip(i);
        if (!strip || !momentaryStutterStripArmed[idx] || !strip->hasAudio() || !strip->isPlaying())
            continue;

        saved.valid = true;
        saved.pan = strip->getPan();
        saved.playbackSpeed = strip->getPlaybackSpeed();
        saved.pitchShift = strip->getPitchShift();
        saved.filterEnabled = strip->isFilterEnabled();
        saved.filterFrequency = strip->getFilterFrequency();
        saved.filterResonance = strip->getFilterResonance();
        saved.filterMorph = strip->getFilterMorph();
        saved.filterAlgorithm = strip->getFilterAlgorithm();
    }

    momentaryStutterMacroBaselineCaptured = true;
    momentaryStutterMacroCapturePending = false;
}

void MlrVSTAudioProcessor::applyMomentaryStutterMacro(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine
        || !momentaryStutterHoldActive
        || momentaryStutterPlaybackActive.load(std::memory_order_acquire) == 0)
        return;

    if (!posInfo.getPpqPosition().hasValue())
        return;

    const double ppqNow = *posInfo.getPpqPosition();
    if (!std::isfinite(ppqNow))
        return;

    if (momentaryStutterMacroCapturePending || !momentaryStutterMacroBaselineCaptured)
        captureMomentaryStutterMacroBaseline();
    if (!momentaryStutterMacroBaselineCaptured)
        return;

    uint8_t comboMask = static_cast<uint8_t>(momentaryStutterButtonMask.load(std::memory_order_acquire) & 0x7f);
    if (comboMask == 0)
        comboMask = stutterButtonBitFromColumn(momentaryStutterActiveDivisionButton);
    if (comboMask == 0)
        return;

    const int bitCount = countStutterBits(comboMask);
    const int highestBit = highestStutterBit(comboMask);
    const int lowestBit = lowestStutterBit(comboMask);
    const bool comboChanged = (comboMask != momentaryStutterLastComboMask);
    const int seed = (static_cast<int>(comboMask) * 97)
        + (bitCount * 19)
        + (highestBit * 11)
        + (lowestBit * 5);
    const int variant = seed % 8;
    const bool singleButton = (bitCount <= 1);
    const bool multiButton = (bitCount > 1);
    const bool twoButton = (bitCount == 2);
    const bool allowPitchSpeedMacro = (bitCount > 2);
    const bool allowPitchMacro = (bitCount > 2);
    const bool applySpeedMacro = (bitCount > 1);
    const bool threeButton = (bitCount == 3);
    const bool hardStepMode = (variant >= 4);

    int lengthBars = 1 + ((seed / 13) % 4);
    if (threeButton)
    {
        // Keep 3-button scenes longer and include explicit 2-bar phrases.
        lengthBars = (((seed / 31) + highestBit + lowestBit) & 0x1) == 0 ? 2 : 4;
    }
    const double cycleBeats = 4.0 * static_cast<double>(lengthBars);
    if (cycleBeats <= 0.0 || !std::isfinite(cycleBeats))
        return;

    const double cycleBeatPosRaw = std::fmod(ppqNow - momentaryStutterMacroStartPpq, cycleBeats);
    const double cycleBeatPos = cycleBeatPosRaw < 0.0 ? cycleBeatPosRaw + cycleBeats : cycleBeatPosRaw;
    const double phase = wrapUnitPhase(cycleBeatPos / cycleBeats);
    const int threeButtonContour = threeButton
        ? (((seed / 29) + variant + highestBit + (lowestBit * 2)) % 4)
        : 0;
    int stepsPerBar = 8;
    if (multiButton)
    {
        const int rhythmClass = ((seed / 7) + highestBit + lowestBit) % 3;
        if (rhythmClass == 1)
            stepsPerBar = 10;
        else if (rhythmClass == 2)
            stepsPerBar = 7;
    }
    const int totalSteps = juce::jmax(8, stepsPerBar * lengthBars);
    const int stepIndex = juce::jlimit(0, totalSteps - 1, static_cast<int>(std::floor(phase * static_cast<double>(totalSteps))));
    const int stepLoop = stepIndex % 8;
    const float normStep = static_cast<float>(stepLoop) / 7.0f;

    const uint8_t maskBit10 = static_cast<uint8_t>(1u << 1);
    const uint8_t maskBit12 = static_cast<uint8_t>(1u << 3);
    const uint8_t maskBit13 = static_cast<uint8_t>(1u << 4);
    const uint8_t maskBit15 = static_cast<uint8_t>(1u << 6);
    const uint8_t maskBit11 = static_cast<uint8_t>(1u << 2);
    const bool combo10And13 = (comboMask == static_cast<uint8_t>(maskBit10 | maskBit13));
    const bool combo11And13 = (comboMask == static_cast<uint8_t>(maskBit11 | maskBit13));
    const bool combo12And13And15 = (comboMask == static_cast<uint8_t>(maskBit12 | maskBit13 | maskBit15));
    const bool hasTopStutterBit = ((comboMask & maskBit15) != 0);
    const float comboIntensity = juce::jlimit(0.25f, 1.0f, 0.34f + (0.16f * static_cast<float>(bitCount - 1)));
    const double heldBeatsRaw = juce::jmax(0.0, ppqNow - momentaryStutterMacroStartPpq);
    const float heldRamp = juce::jlimit(0.0f, 1.0f, static_cast<float>(heldBeatsRaw / 8.0));

    float shapeIntensity = 1.0f;
    float speedMult = 1.0f;
    float panPattern = 0.0f;
    float pitchPattern = 0.0f;
    float cutoffNorm = 0.85f;
    float targetResonance = 1.2f;
    float targetMorph = 0.25f;
    float panDepthShape = 1.0f;
    float twoButtonSemitoneStep = 0.0f;
    float twoButtonSemitoneSpeedRatio = 1.0f;
    bool twoButtonUseFilter = true;
    bool twoButtonDirectionUp = true;
    int twoButtonStepAbs = 0;
    double dynamicStutterDivisionBeats = stutterDivisionBeatsFromBitForMacro(highestBit, multiButton);

    if (variant < 4)
    {
        // Smooth musical movement modes (continuous phase paths).
        const double fastPhase = wrapUnitPhase(phase * static_cast<double>(2 + ((seed >> 2) % 5)));
        const double panPhase = wrapUnitPhase(phase * static_cast<double>(1 + ((seed >> 4) % 4)));
        const double filterPhase = wrapUnitPhase(phase * static_cast<double>(1 + ((seed >> 6) % 3)));
        const double tri = 1.0 - std::abs((phase * 2.0) - 1.0);
        const double triSigned = (tri * 2.0) - 1.0;
        const double sawSigned = (phase * 2.0) - 1.0;
        const double sine = std::sin(juce::MathConstants<double>::twoPi * phase);
        const double sineFast = std::sin(juce::MathConstants<double>::twoPi * fastPhase);
        const double panSine = std::sin(juce::MathConstants<double>::twoPi * panPhase);
        const double filterTri = 1.0 - std::abs((filterPhase * 2.0) - 1.0);

        switch (variant)
        {
            case 0: // riser
                shapeIntensity = juce::jlimit(0.18f, 1.0f, static_cast<float>(phase));
                speedMult = juce::jlimit(0.70f, 2.40f, static_cast<float>(0.95 + (0.95 * phase) + (0.18 * sineFast)));
                panPattern = static_cast<float>(0.48 * panSine);
                pitchPattern = static_cast<float>(-1.0 + (11.5 * phase) + (1.8 * sineFast));
                cutoffNorm = static_cast<float>(0.18 + (0.78 * phase));
                targetResonance = static_cast<float>(0.9 + (2.9 * filterTri));
                targetMorph = static_cast<float>(0.12 + (0.58 * filterPhase));
                break;
            case 1: // faller
                shapeIntensity = juce::jlimit(0.18f, 1.0f, static_cast<float>(1.0 - phase));
                speedMult = juce::jlimit(0.70f, 2.30f, static_cast<float>(1.90 - (1.00 * phase) + (0.16 * sine)));
                panPattern = static_cast<float>(0.72 * triSigned);
                pitchPattern = static_cast<float>(8.0 - (14.0 * phase) + (1.3 * sine));
                cutoffNorm = static_cast<float>(0.92 - (0.70 * phase));
                targetResonance = static_cast<float>(1.1 + (3.1 * phase));
                targetMorph = static_cast<float>(0.88 - (0.62 * filterPhase));
                break;
            case 2: // swirl
                shapeIntensity = juce::jlimit(0.20f, 1.0f, static_cast<float>(tri));
                speedMult = juce::jlimit(0.75f, 2.15f, static_cast<float>(1.0
                    + (0.42 * std::sin(juce::MathConstants<double>::twoPi * phase * 2.0))
                    + (0.14 * sineFast)));
                panPattern = static_cast<float>(0.80 * std::sin(juce::MathConstants<double>::twoPi * (panPhase * 2.0)));
                pitchPattern = static_cast<float>((6.0 * sine) + (3.0 * std::sin(juce::MathConstants<double>::twoPi * (phase + 0.25))));
                cutoffNorm = static_cast<float>(0.24 + (0.66 * filterTri));
                targetResonance = static_cast<float>(0.9 + (2.5 * wrapUnitPhase(filterPhase * 2.0)));
                targetMorph = static_cast<float>(0.50 + (0.40 * std::sin(juce::MathConstants<double>::twoPi * filterPhase)));
                break;
            case 3:
            default: // surge
                shapeIntensity = juce::jlimit(0.22f, 1.0f, static_cast<float>(0.55 + (0.45 * std::abs(sineFast))));
                speedMult = juce::jlimit(0.70f, 2.40f, static_cast<float>(1.0 + (0.95 * triSigned) + (0.14 * sineFast)));
                panPattern = static_cast<float>(0.90 * sawSigned);
                pitchPattern = static_cast<float>((9.0 * sine) + (4.5 * triSigned));
                cutoffNorm = static_cast<float>(0.14 + (0.80 * wrapUnitPhase(phase + (0.25 * juce::jmax(0.0, sine)))));
                targetResonance = static_cast<float>(1.0 + (3.0 * wrapUnitPhase(filterPhase + (0.20 * triSigned))));
                targetMorph = static_cast<float>(wrapUnitPhase((0.40 * phase) + (0.60 * filterPhase)));
                break;
        }
    }
    else
    {
        // Hard step modes (deterministic rhythmic snapshots).
        static constexpr std::array<std::array<float, 8>, 8> kSpeedPatterns{{
            {{ 1.00f, 1.25f, 1.50f, 1.75f, 1.50f, 1.25f, 1.00f, 0.85f }},
            {{ 1.00f, 0.90f, 1.10f, 1.35f, 1.60f, 1.35f, 1.10f, 0.90f }},
            {{ 1.00f, 1.12f, 1.25f, 1.38f, 1.50f, 1.62f, 1.75f, 1.50f }},
            {{ 1.00f, 1.50f, 1.00f, 1.25f, 1.00f, 1.75f, 1.00f, 1.50f }},
            {{ 1.00f, 1.15f, 1.30f, 1.45f, 1.30f, 1.15f, 1.00f, 0.90f }},
            {{ 1.00f, 0.85f, 1.00f, 1.35f, 1.00f, 1.55f, 1.20f, 1.00f }},
            {{ 1.00f, 1.20f, 1.45f, 1.20f, 0.95f, 1.20f, 1.45f, 1.70f }},
            {{ 1.00f, 1.33f, 1.67f, 1.33f, 1.00f, 0.90f, 1.10f, 1.30f }}
        }};
        static constexpr std::array<std::array<float, 8>, 8> kPanPatterns{{
            {{ -1.00f, 1.00f, -0.80f, 0.80f, -0.60f, 0.60f, -0.35f, 0.35f }},
            {{ -0.70f, -0.30f, 0.30f, 0.70f, 1.00f, 0.70f, 0.30f, -0.30f }},
            {{ -1.00f, -0.60f, -0.20f, 0.20f, 0.60f, 1.00f, 0.40f, -0.20f }},
            {{ -1.00f, 1.00f, -1.00f, 1.00f, -0.50f, 0.50f, -0.20f, 0.20f }},
            {{ -0.25f, -0.75f, -1.00f, -0.50f, 0.50f, 1.00f, 0.75f, 0.25f }},
            {{ -0.90f, -0.20f, 0.90f, 0.20f, -0.90f, -0.20f, 0.90f, 0.20f }},
            {{ -0.40f, 0.40f, -0.70f, 0.70f, -1.00f, 1.00f, -0.60f, 0.60f }},
            {{ -1.00f, -0.50f, 0.00f, 0.50f, 1.00f, 0.50f, 0.00f, -0.50f }}
        }};
        static constexpr std::array<std::array<float, 8>, 8> kPitchPatterns{{
            {{ 0.0f, 2.0f, 5.0f, 7.0f, 10.0f, 7.0f, 5.0f, 2.0f }},
            {{ 0.0f, -2.0f, 3.0f, 5.0f, 8.0f, 5.0f, 3.0f, -2.0f }},
            {{ 0.0f, 3.0f, 7.0f, 10.0f, 12.0f, 10.0f, 7.0f, 3.0f }},
            {{ 0.0f, 5.0f, 0.0f, 7.0f, 0.0f, 10.0f, 0.0f, 12.0f }},
            {{ 0.0f, 2.0f, 4.0f, 7.0f, 9.0f, 7.0f, 4.0f, 2.0f }},
            {{ 0.0f, -3.0f, 0.0f, 4.0f, 7.0f, 4.0f, 0.0f, -3.0f }},
            {{ 0.0f, 1.0f, 5.0f, 8.0f, 12.0f, 8.0f, 5.0f, 1.0f }},
            {{ 0.0f, 4.0f, 7.0f, 11.0f, 7.0f, 4.0f, 2.0f, 0.0f }}
        }};

        const int patternBank = ((seed / 5) + (bitCount * 3) + highestBit + lowestBit) % 8;
        const auto& speedPattern = kSpeedPatterns[static_cast<size_t>((variant + patternBank) % 8)];
        const auto& panPatternTable = kPanPatterns[static_cast<size_t>((variant + highestBit + patternBank) % 8)];
        const auto& pitchPatternTable = kPitchPatterns[static_cast<size_t>((variant + lowestBit + (patternBank * 2)) % 8)];

        switch (variant % 4)
        {
            case 0: shapeIntensity = juce::jlimit(0.15f, 1.0f, normStep); break; // rise
            case 1: shapeIntensity = juce::jlimit(0.15f, 1.0f, 1.0f - normStep); break; // fall
            case 2: shapeIntensity = juce::jlimit(0.15f, 1.0f, 1.0f - std::abs((normStep * 2.0f) - 1.0f)); break; // triangle
            case 3:
            default: shapeIntensity = (stepLoop & 1) == 0 ? 1.0f : 0.45f; break; // pulse
        }

        speedMult = speedPattern[static_cast<size_t>(stepLoop)];
        panPattern = panPatternTable[static_cast<size_t>(stepLoop)];
        pitchPattern = pitchPatternTable[static_cast<size_t>(stepLoop)];
        cutoffNorm = juce::jlimit(0.10f, 1.0f, 0.25f + (0.70f * normStep));
        targetResonance = 0.9f + (3.2f * shapeIntensity);
        targetMorph = juce::jlimit(0.05f, 0.95f, 0.10f + (0.80f * normStep));

        // Hard-step variants escalate while held to create stronger breakdown/riser motion.
        const float hardExtreme = juce::jlimit(1.0f, 2.1f, 1.0f + (1.1f * heldRamp));
        shapeIntensity = juce::jlimit(0.15f, 1.0f, shapeIntensity + (0.50f * heldRamp));
        speedMult = 1.0f + ((speedMult - 1.0f) * hardExtreme);
        panPattern = juce::jlimit(-1.0f, 1.0f, panPattern * (1.0f + (0.45f * heldRamp)));
        pitchPattern = juce::jlimit(-18.0f, 18.0f, pitchPattern * (1.0f + (0.95f * heldRamp)));
        targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (2.1f * heldRamp));
        targetMorph = juce::jlimit(0.02f, 0.98f, targetMorph + (0.14f * heldRamp));
    }

    if (allowPitchSpeedMacro)
    {
        // Hard-step speed scenes are always available for >2-button holds.
        static constexpr std::array<std::array<float, 8>, 4> kHardSpeedScenes {{
            {{ 0.30f, 0.55f, 1.15f, 2.20f, 3.40f, 2.40f, 1.20f, 0.45f }},
            {{ 1.00f, 0.35f, 0.70f, 1.60f, 3.20f, 2.20f, 1.10f, 0.40f }},
            {{ 3.40f, 2.40f, 1.60f, 1.00f, 0.50f, 0.75f, 1.35f, 2.20f }},
            {{ 0.28f, 0.50f, 0.85f, 1.50f, 2.60f, 3.60f, 1.80f, 0.42f }}
        }};
        const int hardSceneIdx = ((seed / 9) + highestBit + (lowestBit * 2)) % 4;
        const float hardStepSpeed = kHardSpeedScenes[static_cast<size_t>(hardSceneIdx)][static_cast<size_t>(stepLoop)];
        float hardMix = (variant >= 4) ? 0.76f : 0.42f;
        hardMix += 0.22f * heldRamp;
        if (threeButton)
            hardMix += 0.12f;
        hardMix = juce::jlimit(0.0f, 1.0f, hardMix);
        speedMult = juce::jmap(hardMix, speedMult, hardStepSpeed);
    }

    if (threeButton)
    {
        // 3-button combos start from a stronger base before contour shaping.
        shapeIntensity = juce::jlimit(0.2f, 1.0f, shapeIntensity + 0.20f + (0.25f * heldRamp));
        speedMult = juce::jlimit(0.25f, 4.0f, speedMult * (1.08f + (0.42f * heldRamp)));
        panPattern = juce::jlimit(-1.0f, 1.0f, panPattern * (1.20f + (0.35f * heldRamp)));
        pitchPattern = juce::jlimit(-14.0f, 14.0f, pitchPattern * (1.04f + (0.18f * heldRamp)));
    }

    if (!allowPitchSpeedMacro && hardStepMode)
    {
        // Hard-step depth envelope for 1/2-button stutters.
        // 1-button: subtle pan-only growth.
        // 2-button: stronger growth for pan + filter shape over hold time.
        const float hardDepth = juce::jlimit(0.0f, 1.0f, std::pow(heldRamp, 1.35f));
        if (singleButton)
        {
            panDepthShape = juce::jlimit(0.08f, 0.24f, 0.08f + (0.16f * hardDepth));
        }
        else
        {
            const float twoButtonDepth = juce::jlimit(0.28f, 1.0f, 0.28f + (0.72f * hardDepth));
            panDepthShape = twoButtonDepth;
            const float stepPolarity = ((stepLoop & 1) == 0) ? 1.0f : -1.0f;
            cutoffNorm = juce::jlimit(0.0f, 1.0f, cutoffNorm + (0.16f * twoButtonDepth * stepPolarity));
            targetMorph = juce::jlimit(0.0f, 1.0f, targetMorph + (0.18f * twoButtonDepth * stepPolarity));
            targetResonance = juce::jlimit(0.2f, 2.1f, targetResonance + (0.45f * twoButtonDepth));
        }
    }
    else if (singleButton)
    {
        // One-button stutter should remain mostly clean and centered.
        panDepthShape = 0.10f;
    }

    if (twoButton)
    {
        // Two-finger mode:
        // - use fixed PPQ-safe retrigger divisions,
        // - include both slow linear ramps and dramatic fast exponential ramps,
        // - always starts from the current speed baseline at step 0.
        const int twoButtonMode = ((seed / 7) + (highestBit * 3) + lowestBit) & 0x7;
        twoButtonDirectionUp = ((twoButtonMode & 0x1) == 0);
        twoButtonUseFilter = (twoButtonMode <= 3); // Keep higher-intensity modes filter-free.
        const float phaseNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(phase));

        // Keep some slower linear ramps and add aggressive/faster variants.
        if (twoButtonMode <= 1)
            dynamicStutterDivisionBeats = 0.5;   // 1/8 (slower)
        else if (twoButtonMode <= 3)
            dynamicStutterDivisionBeats = 0.25;  // 1/16 (fast)
        else if (twoButtonMode <= 5)
            dynamicStutterDivisionBeats = 0.125; // 1/32 (dramatic)
        else
            dynamicStutterDivisionBeats = 0.0625; // 1/64 (extreme)

        const double elapsedBeats = juce::jmax(0.0, ppqNow - momentaryStutterMacroStartPpq);
        const double stepPos = elapsedBeats / juce::jmax(0.03125, dynamicStutterDivisionBeats);
        const int globalTwoButtonStep = juce::jmax(0, static_cast<int>(std::floor((std::isfinite(stepPos) ? stepPos : 0.0) + 1.0e-6)));
        if (comboChanged || !momentaryStutterTwoButtonStepBaseValid)
        {
            momentaryStutterTwoButtonStepBase = globalTwoButtonStep;
            momentaryStutterTwoButtonStepBaseValid = true;
        }
        twoButtonStepAbs = juce::jmax(0, globalTwoButtonStep - momentaryStutterTwoButtonStepBase);
        const int semitoneStride = (twoButtonMode >= 4) ? 2 : 1;
        // Slow ramps can span up to 3 octaves; fast ramps are capped to 1 octave.
        const int twoButtonMaxSemitones = (twoButtonMode <= 1) ? 36 : 12;
        int pacedStepAbs = twoButtonStepAbs;
        if (twoButtonMode >= 2)
        {
            // Dynamic pacing: faster stutter divisions ramp pitch more slowly per trigger.
            const float paceScale = juce::jlimit(0.125f, 1.0f,
                static_cast<float>(dynamicStutterDivisionBeats / 0.5));
            const float pacedContinuous = static_cast<float>(twoButtonStepAbs) * paceScale;
            pacedStepAbs = juce::jmax(0, static_cast<int>(std::floor(pacedContinuous + 1.0e-4f)));
        }

        const int linearSemitoneStep = juce::jlimit(0, twoButtonMaxSemitones, pacedStepAbs * semitoneStride);
        int semitoneStep = linearSemitoneStep;
        if (twoButtonMode >= 2)
        {
            // Fast modes use exponential semitone rise/fall so ramps feel more dramatic.
            const float expoK = (twoButtonMode >= 6) ? 0.74f
                : (twoButtonMode >= 4 ? 0.58f
                                      : (twoButtonMode >= 2 ? 0.36f : 0.30f));
            const float expoNorm = juce::jlimit(0.0f, 1.0f,
                1.0f - std::exp(-expoK * static_cast<float>(pacedStepAbs)));
            const int maxExpoStep = juce::jmax(1, twoButtonMaxSemitones / semitoneStride);
            const int expoStepIndex = juce::jlimit(0, maxExpoStep, static_cast<int>(std::round(expoNorm * static_cast<float>(maxExpoStep))));
            const int expoSemitoneStep = juce::jlimit(0, twoButtonMaxSemitones, expoStepIndex * semitoneStride);
            semitoneStep = juce::jmax(linearSemitoneStep, expoSemitoneStep);
        }
        twoButtonSemitoneStep = static_cast<float>(twoButtonDirectionUp ? semitoneStep : -semitoneStep);

        const float panDepthStep = juce::jlimit(0.0f, 1.0f, static_cast<float>(semitoneStep) / 16.0f);
        panDepthShape = panDepthStep;
        // Quantized semitone-step speed ratio (1 or 2 semitones per retrigger depending on mode).
        twoButtonSemitoneSpeedRatio = std::pow(2.0f, twoButtonSemitoneStep / 12.0f);

        if (twoButtonUseFilter)
        {
            // Two-finger filter curves always start OPEN at stutter start.
            // mode 0: LP closes over phrase; mode 1: HP rises from open over phrase.
            const float curve = std::pow(phaseNorm, 1.35f);
            targetResonance = juce::jlimit(0.2f, 1.3f, 0.72f + (0.28f * curve));
            if (twoButtonMode == 0)
            {
                targetMorph = 0.0f; // low-pass
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (0.72f * curve)); // open at start
            }
            else
            {
                targetMorph = 1.0f; // high-pass
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 0.04f + (0.64f * curve)); // open at start
            }
        }
    }
    else
    {
        momentaryStutterTwoButtonStepBaseValid = false;
        momentaryStutterTwoButtonStepBase = 0;
    }

    // Multi-button combos add infinite ramp movement layers (looping every cycle)
    // that continue until release: retrigger-rate sweeps + coordinated speed/filter ramps.
    if (multiButton && !twoButton)
    {
        const float phaseNorm = static_cast<float>(phase);
        const float rampUp = juce::jlimit(0.0f, 1.0f, phaseNorm);
        const float rampDown = 1.0f - rampUp;
        const float rampPingPong = juce::jlimit(0.0f, 1.0f, static_cast<float>(1.0 - std::abs((phase * 2.0) - 1.0)));
        const float heldDrive = juce::jlimit(0.20f, 1.0f, 0.35f + (0.65f * heldRamp));

        const double baseDivision = juce::jlimit(0.0625, 1.0, dynamicStutterDivisionBeats);
        const double minFastDivision = allowPitchSpeedMacro ? 0.0625 : 0.125;
        const double fastDivision = juce::jlimit(minFastDivision, 1.0, baseDivision * (threeButton ? 0.30 : 0.42));
        const double slowDivision = juce::jlimit(0.0625, 2.0, baseDivision * (threeButton ? 2.25 : 1.85));

        const int rampMode = ((seed / 17) + bitCount + highestBit + lowestBit) % 4;
        switch (rampMode)
        {
            case 0: // accel + high-pass rise
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, fastDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (1.35f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, amt);
                targetMorph = 1.0f; // High-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (1.0f * amt));
                break;
            }
            case 1: // accel + low-pass fall
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, fastDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (1.20f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                targetMorph = 0.0f; // Low-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.7f * amt));
                break;
            }
            case 2: // decel + low-pass fall
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, slowDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f - (0.58f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                targetMorph = 0.0f; // Low-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.6f * amt));
                break;
            }
            case 3:
            default: // infinite up/down ping-pong ramp
            {
                const float amt = rampPingPong * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), slowDivision, fastDivision);
                if (allowPitchSpeedMacro)
                {
                    const float swing = ((rampPingPong * 2.0f) - 1.0f) * heldDrive;
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (0.65f * swing)));
                }

                // Alternate LP/HP flavor each half cycle while maintaining a continuous ramp.
                if (rampUp >= rampDown)
                {
                    cutoffNorm = juce::jlimit(0.0f, 1.0f, amt);
                    targetMorph = 1.0f; // High-pass
                }
                else
                {
                    cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                    targetMorph = 0.0f; // Low-pass
                }
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.8f * amt));
                break;
            }
        }
    }

    if (threeButton)
    {
        // Musical 3-button contours: exponential risers/fallers and curved macro motion.
        const float phaseNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(phase));
        const bool fastContour = (threeButtonContour <= 1);
        const float expPowerFast = fastContour
            ? (0.62f + (0.34f * heldRamp))
            : (1.12f + (0.48f * heldRamp));
        const float expPowerSlow = fastContour
            ? (0.78f + (0.30f * heldRamp))
            : (1.04f + (0.44f * heldRamp));
        const float expRise = std::pow(phaseNorm, expPowerFast);
        const float expFall = std::pow(1.0f - phaseNorm, expPowerFast);
        const float arc = (phaseNorm < 0.5f)
            ? std::pow(phaseNorm * 2.0f, expPowerSlow)
            : std::pow((1.0f - phaseNorm) * 2.0f, expPowerSlow);
        const float contourDrive = juce::jlimit(0.0f, 1.0f, 0.38f + (0.62f * heldRamp));
        const double longPatternSlow = fastContour
            ? (lengthBars >= 2 ? 1.58 : 1.26)
            : (lengthBars >= 2 ? 2.04 : 1.52);
        const double longPatternFast = fastContour
            ? (lengthBars >= 2 ? 0.19 : 0.28)
            : (lengthBars >= 2 ? 0.40 : 0.50);

        switch (threeButtonContour)
        {
            case 0: // Exponential riser
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(expRise, 1.00f, 4.00f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(expRise, -1.0f, 14.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(expRise, 0.12f, 0.70f));
                targetMorph = 1.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.72f + (0.72f * expRise));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(expRise, 0.02f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(expRise),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * longPatternSlow),
                    juce::jmax(0.0625, dynamicStutterDivisionBeats * longPatternFast));
                break;
            }
            case 1: // Exponential faller
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(expFall, 0.55f, 3.85f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(expFall, -13.0f, 10.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(expFall, 0.18f, 0.92f));
                targetMorph = 0.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.68f + (0.64f * expFall));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(expFall, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(expFall),
                    juce::jmax(0.0625, dynamicStutterDivisionBeats * longPatternFast),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * longPatternSlow));
                break;
            }
            case 2: // Rise then fall arc
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(arc, 0.70f, 3.95f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(arc, -5.0f, 13.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(arc, 0.16f, 0.76f));
                targetMorph = (phaseNorm < 0.5f) ? 1.0f : 0.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.72f + (0.58f * arc));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(arc, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(arc),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * (longPatternSlow - 0.20)),
                    juce::jmax(0.0625, dynamicStutterDivisionBeats * (longPatternFast + 0.05)));
                break;
            }
            case 3:
            default: // Fall then rise arc
            {
                const float invArc = 1.0f - arc;
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(invArc, 0.62f, 3.70f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(invArc, -11.0f, 10.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(invArc, 0.20f, 0.88f));
                targetMorph = (phaseNorm < 0.5f) ? 0.0f : 1.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.66f + (0.58f * invArc));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(invArc, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(invArc),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * (longPatternSlow - 0.10)),
                    juce::jmax(0.0625, dynamicStutterDivisionBeats * (longPatternFast + 0.08)));
                break;
            }
        }

        // Make contour ramps react faster as the hold deepens.
        speedMult = juce::jlimit(0.25f, 4.0f, speedMult * (1.0f + (0.35f * contourDrive)));
    }

    // Musical safety guard:
    // 2-button combos should stay expressive but avoid ultra-harsh ringing/noise at high stutter rates.
    if (!allowPitchSpeedMacro)
    {
        const double minDivision = twoButton ? 0.0625 : 0.125;
        dynamicStutterDivisionBeats = juce::jlimit(minDivision, 4.0, dynamicStutterDivisionBeats);
        targetResonance = juce::jlimit(0.2f, 1.4f, targetResonance);
    }

    // High-density col15 combos can become brittle/noisy when all macro dimensions
    // align at the same time; keep them in a musical envelope.
    if (allowPitchSpeedMacro && hasTopStutterBit)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.0833333333, 4.0, dynamicStutterDivisionBeats);
        speedMult = juce::jlimit(0.60f, 2.0f, speedMult);
        pitchPattern = juce::jlimit(-8.0f, 8.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, 2.4f, targetResonance);
    }

    // Explicitly tame known harsh combinations.
    if (combo10And13)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        targetMorph = 0.0f;
        targetResonance = juce::jlimit(0.2f, 1.2f, targetResonance);
    }

    if (combo11And13)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        targetMorph = 0.0f;
        targetResonance = juce::jlimit(0.2f, 1.1f, targetResonance);
    }

    if (combo12And13And15)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.0833333333, 4.0, dynamicStutterDivisionBeats);
        speedMult = juce::jlimit(0.70f, 1.60f, speedMult);
        pitchPattern = juce::jlimit(-6.0f, 6.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, 1.8f, targetResonance);
    }

    if (multiButton)
    {
        static constexpr std::array<double, 5> kTwoButtonGrid { 1.0, 0.5, 0.25, 0.125, 0.0625 };
        static constexpr std::array<double, 5> kThreeButtonGrid { 1.0, 0.5, 0.25, 0.125, 0.0625 };
        static constexpr std::array<double, 4> kDenseButtonGrid { 0.5, 0.25, 0.125, 0.0625 };

        if (bitCount == 2)
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kTwoButtonGrid);
        else if (bitCount == 3)
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kThreeButtonGrid);
        else
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kDenseButtonGrid);
    }

    const bool veryFastDivision = dynamicStutterDivisionBeats <= 0.1250001;
    const bool ultraFastDivision = dynamicStutterDivisionBeats <= 0.0835001;
    if (multiButton && veryFastDivision)
    {
        const float speedFloor = ultraFastDivision ? 0.72f : 0.60f;
        const float speedCeil = allowPitchSpeedMacro
            ? (ultraFastDivision ? 1.95f : (threeButton ? 2.60f : 2.20f))
            : (twoButton ? (ultraFastDivision ? 2.15f : 2.85f) : 1.25f);
        speedMult = juce::jlimit(speedFloor, speedCeil, speedMult);
        pitchPattern = juce::jlimit(-6.0f, 6.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, ultraFastDivision ? 0.85f : 1.05f, targetResonance);
        if (targetMorph > 0.70f)
            targetMorph = ultraFastDivision ? 0.58f : 0.70f;
    }

    if (multiButton && targetMorph > 0.82f && cutoffNorm > 0.78f)
        targetResonance = juce::jmin(targetResonance, 0.9f);

    if (multiButton)
    {
        // Keep cutoff+morph inside audible zones to avoid click-only/no-audio states.
        if (targetMorph >= 0.70f)
            cutoffNorm = juce::jlimit(0.04f, 0.72f, cutoffNorm);
        else if (targetMorph <= 0.30f)
            cutoffNorm = juce::jlimit(0.16f, 0.98f, cutoffNorm);
        else
            cutoffNorm = juce::jlimit(0.08f, 0.94f, cutoffNorm);

        if ((targetMorph >= 0.72f && cutoffNorm >= 0.62f)
            || (targetMorph <= 0.16f && cutoffNorm <= 0.22f))
            targetResonance = juce::jmin(targetResonance, 0.82f);
    }

    if (applySpeedMacro && !twoButton)
    {
        // Stutter speed is hard-stepped by PPQ phase step index (no smooth glides).
        const float cycleStepNorm = (totalSteps > 1)
            ? juce::jlimit(0.0f, 1.0f, static_cast<float>(stepIndex) / static_cast<float>(totalSteps - 1))
            : 0.0f;
        const int rampShape = threeButton ? threeButtonContour : (variant & 0x3);
        float rampNorm = cycleStepNorm;
        switch (rampShape)
        {
            case 0: // up
                rampNorm = cycleStepNorm;
                break;
            case 1: // down
                rampNorm = 1.0f - cycleStepNorm;
                break;
            case 2: // up then down
                rampNorm = (cycleStepNorm < 0.5f)
                    ? (cycleStepNorm * 2.0f)
                    : ((1.0f - cycleStepNorm) * 2.0f);
                break;
            case 3: // down then up
            default:
                rampNorm = (cycleStepNorm < 0.5f)
                    ? (1.0f - (cycleStepNorm * 2.0f))
                    : ((cycleStepNorm - 0.5f) * 2.0f);
                break;
        }
        const float expShape = threeButton
            ? (0.90f + (0.95f * heldRamp))
            : (1.20f + (1.10f * heldRamp) + (twoButton ? 0.20f : 0.0f));
        const float shapedRamp = std::pow(juce::jlimit(0.0f, 1.0f, rampNorm), expShape);
        const float minHardSpeedMult = threeButton ? 0.45f : 0.55f;
        const float maxHardSpeedMult = threeButton ? 3.9f : 3.1f;
        const float hardStepSpeedMult = juce::jmap(shapedRamp, minHardSpeedMult, maxHardSpeedMult);
        const float hardStepBlend = threeButton ? 0.96f : (twoButton ? 0.88f : 0.84f);
        speedMult = juce::jmap(hardStepBlend, speedMult, hardStepSpeedMult);
    }

    const float intensity = juce::jlimit(0.20f, 1.0f, comboIntensity * shapeIntensity);
    const float speedIntensityScale = juce::jlimit(0.35f, 1.0f, 0.42f + (0.58f * intensity));
    const float shapedSpeedMult = twoButton
        ? juce::jlimit(0.03125f, 8.0f, twoButtonSemitoneSpeedRatio)
        : (1.0f + ((speedMult - 1.0f) * speedIntensityScale));
    const float pitchOffsetBasePattern = juce::jlimit(-12.0f, 12.0f, pitchPattern * (0.55f + (0.30f * intensity)));
    // Keep pitch secondary: speed carries the primary riser/faller motion.
    const float speedToPitchDepth = allowPitchMacro ? (threeButton ? 3.0f : 2.0f) : 0.0f;
    const float pitchOffsetFromSpeedShape = juce::jlimit(-12.0f, 12.0f, (shapedSpeedMult - 1.0f) * speedToPitchDepth);
    const float pitchOffsetBase = juce::jlimit(
        -12.0f, 12.0f, pitchOffsetBasePattern + ((allowPitchMacro && !twoButton) ? pitchOffsetFromSpeedShape : 0.0f));

    // Pan is always hard-stepped and locked to the active stutter subdivision.
    const double panDivisionBeats = juce::jmax(0.03125, dynamicStutterDivisionBeats);
    const double panStepPos = (ppqNow - momentaryStutterMacroStartPpq) / panDivisionBeats;
    const int panStepIndex = static_cast<int>(std::floor(std::isfinite(panStepPos) ? panStepPos : 0.0));
    const int panMode = ((seed / 23) + bitCount + highestBit + lowestBit) & 0x3;
    static constexpr std::array<float, 8> kPanSeqA { -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
    static constexpr std::array<float, 8> kPanSeqB { -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
    float panHardStep = -1.0f;
    switch (panMode)
    {
        case 0:
            panHardStep = (panStepIndex & 1) ? 1.0f : -1.0f;
            break;
        case 1:
            panHardStep = ((panStepIndex >> 1) & 1) ? 1.0f : -1.0f;
            break;
        case 2:
            panHardStep = kPanSeqA[static_cast<size_t>(juce::jmax(0, panStepIndex) & 7)];
            break;
        case 3:
        default:
            panHardStep = kPanSeqB[static_cast<size_t>(juce::jmax(0, panStepIndex) & 7)];
            break;
    }
    if (twoButton)
        panHardStep = (panStepIndex & 1) ? 1.0f : -1.0f;
    if (panPattern < 0.0f)
        panHardStep = -panHardStep;
    const float panDriveBase = juce::jlimit(0.72f, 1.0f,
        0.72f + (0.28f * intensity) + (threeButton ? 0.10f : 0.0f) + (veryFastDivision ? 0.08f : 0.0f));
    float panDepth = 1.0f;
    if (threeButton)
        panDepth = juce::jlimit(0.18f, 1.0f, panDepthShape);
    else if (singleButton)
        panDepth = juce::jlimit(0.05f, 0.28f, panDepthShape);
    else if (twoButton)
        panDepth = juce::jlimit(0.0f, 1.0f, panDepthShape);
    else
        panDepth = juce::jlimit(0.28f, 1.0f, panDepthShape);
    const float panDrive = twoButton
        ? juce::jlimit(0.0f, 1.0f, panDriveBase * panDepth)
        : juce::jlimit(0.18f, 1.0f, panDriveBase * panDepth);
    const float panOffsetBase = juce::jlimit(-1.0f, 1.0f, panHardStep * panDrive);

    cutoffNorm = juce::jlimit(0.0f, 1.0f, cutoffNorm);
    const float resonanceScale = threeButton
        ? juce::jlimit(0.75f, 1.15f, comboIntensity + 0.18f)
        : comboIntensity;
    targetResonance = juce::jlimit(0.2f, threeButton ? 2.4f : 8.0f, targetResonance * resonanceScale);
    targetMorph = juce::jlimit(0.0f, 1.0f, targetMorph);

    auto filterAlgorithm = filterAlgorithmFromIndex((variant + bitCount + highestBit + lowestBit) % 6);
    if (combo10And13 || combo11And13 || combo12And13And15
        || (!allowPitchSpeedMacro && highestBit >= 5 && targetMorph > 0.74f)
        || (multiButton && veryFastDivision))
        filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
    const float targetCutoff = cutoffFromNormalized(cutoffNorm);
    audioEngine->setMomentaryStutterDivision(juce::jlimit(0.03125, 4.0, dynamicStutterDivisionBeats));
    const double speedStepDivisionBeats = juce::jmax(0.03125, dynamicStutterDivisionBeats);
    const double speedStepPos = (ppqNow - momentaryStutterMacroStartPpq) / speedStepDivisionBeats;
    const int speedStepAbs = juce::jmax(0, static_cast<int>(std::floor(std::isfinite(speedStepPos) ? speedStepPos : 0.0)));
    const bool stutterStartStep = (speedStepAbs == 0);
    const bool firstSpeedStep = applySpeedMacro && (speedStepAbs == 0);

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto& saved = momentaryStutterSavedState[idx];
        if (!saved.valid || !momentaryStutterStripArmed[idx])
            continue;

        auto* strip = audioEngine->getStrip(i);
        if (!strip || !strip->hasAudio() || !strip->isPlaying())
            continue;

        const float stripOffset = static_cast<float>(i - (MaxStrips / 2));
        const float stripPanScale = juce::jlimit(0.45f, threeButton ? 1.35f : 1.15f,
            0.65f + (0.08f * static_cast<float>(bitCount)) + (0.05f * static_cast<float>(i)));
        const float stripPitchSpread = (allowPitchSpeedMacro && bitCount > 2) ? (stripOffset * 0.35f) : 0.0f;
        const float stripSpeedSpread = (applySpeedMacro && bitCount > 3) ? (stripOffset * 0.025f) : 0.0f;
        const float stripMorphOffset = static_cast<float>(0.08 * std::sin(
            juce::MathConstants<double>::twoPi * wrapUnitPhase(phase + (0.13 * static_cast<double>(i)))));

        const float savedSpeed = juce::jlimit(0.0f, 4.0f, saved.playbackSpeed);
        const float speedBaseline = savedSpeed;
        const float stutterSpeedFloor = applySpeedMacro
            ? (ultraFastDivision ? 0.72f : (veryFastDivision ? 0.56f : 0.30f))
            : speedBaseline;
        const float stutterSpeedCeil = applySpeedMacro
            ? (ultraFastDivision ? (threeButton ? 2.10f : 1.95f)
                                 : (veryFastDivision ? (threeButton ? 2.80f : 2.35f)
                                                     : (threeButton ? 4.0f : 3.2f)))
            : speedBaseline;
        const float modulatedTargetSpeed = twoButton
            // Two-finger speed always starts at current strip speed and moves
            // up/down in semitone steps relative to that baseline.
            ? juce::jlimit(0.03125f, 8.0f, speedBaseline * shapedSpeedMult)
            : juce::jlimit(stutterSpeedFloor, stutterSpeedCeil,
                (speedBaseline * shapedSpeedMult) + stripSpeedSpread);
        const bool holdBaselineSpeed = twoButton ? (twoButtonStepAbs == 0) : firstSpeedStep;
        const float targetSpeed = holdBaselineSpeed ? speedBaseline : modulatedTargetSpeed;
        if (applySpeedMacro)
            strip->setPlaybackSpeedImmediate(targetSpeed);
        else
            strip->setPlaybackSpeed(speedBaseline);
        strip->setPan(juce::jlimit(-1.0f, 1.0f, saved.pan + (panOffsetBase * stripPanScale)));
        float targetPitch = saved.pitchShift;
        if (twoButton && applySpeedMacro)
        {
            // Guarantee full 3-octave contour even when speed reaches hard limits:
            // carry residual semitone motion into pitch shift.
            const float ratioBase = juce::jmax(0.03125f, speedBaseline);
            const float ratioActual = juce::jmax(0.03125f, targetSpeed / ratioBase);
            const float actualSemitoneFromSpeed = 12.0f * std::log2(ratioActual);
            const float residualSemitone = twoButtonSemitoneStep - actualSemitoneFromSpeed;
            targetPitch = juce::jlimit(-24.0f, 24.0f, saved.pitchShift + residualSemitone);
        }
        else if (allowPitchMacro)
        {
            targetPitch = juce::jlimit(-12.0f, 12.0f, saved.pitchShift + pitchOffsetBase + stripPitchSpread);
        }
        strip->setPitchShift(targetPitch);

        if (singleButton || (twoButton && !twoButtonUseFilter))
        {
            // Clean stutter variants: no filter color.
            strip->setFilterAlgorithm(saved.filterAlgorithm);
            strip->setFilterFrequency(saved.filterFrequency);
            strip->setFilterResonance(saved.filterResonance);
            strip->setFilterMorph(saved.filterMorph);
            strip->setFilterEnabled(saved.filterEnabled);
        }
        else
        {
            strip->setFilterEnabled(true);
            strip->setFilterAlgorithm(filterAlgorithm);
            if (stutterStartStep)
            {
                // Start every stutter with filter fully open and minimum resonance,
                // then apply macro movement from subsequent stutter steps.
                strip->setFilterMorph(0.0f);
                strip->setFilterFrequency(20000.0f);
                strip->setFilterResonance(0.1f);
            }
            else
            {
                strip->setFilterFrequency(targetCutoff);
                strip->setFilterResonance(targetResonance);
                strip->setFilterMorph(juce::jlimit(0.0f, 1.0f, targetMorph + stripMorphOffset));
            }
        }
    }

    momentaryStutterLastComboMask = comboMask;
}

void MlrVSTAudioProcessor::restoreMomentaryStutterMacroBaseline()
{
    if (!audioEngine || !momentaryStutterMacroBaselineCaptured)
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        auto& saved = momentaryStutterSavedState[idx];
        if (!saved.valid)
            continue;

        if (auto* strip = audioEngine->getStrip(i))
        {
            strip->setPan(saved.pan);
            strip->setPlaybackSpeedImmediate(saved.playbackSpeed);
            strip->setPitchShift(saved.pitchShift);
            strip->setFilterAlgorithm(saved.filterAlgorithm);
            strip->setFilterFrequency(saved.filterFrequency);
            strip->setFilterResonance(saved.filterResonance);
            strip->setFilterMorph(saved.filterMorph);
            strip->setFilterEnabled(saved.filterEnabled);
        }

        saved.valid = false;
    }

    momentaryStutterMacroBaselineCaptured = false;
    momentaryStutterMacroCapturePending = false;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
}

juce::File MlrVSTAudioProcessor::getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    return mode == SamplePathMode::Step ? defaultStepDirectories[idx] : defaultLoopDirectories[idx];
}

MlrVSTAudioProcessor::SamplePathMode MlrVSTAudioProcessor::getSamplePathModeForStrip(int stripIndex) const
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return SamplePathMode::Loop;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            return SamplePathMode::Step;
    }

    return SamplePathMode::Loop;
}

juce::File MlrVSTAudioProcessor::getCurrentBrowserDirectoryForStrip(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto isValidDir = [](const juce::File& dir)
    {
        return dir != juce::File() && dir.exists() && dir.isDirectory();
    };

    const auto mode = getSamplePathModeForStrip(stripIndex);
    auto selectedDir = getDefaultSampleDirectory(stripIndex, mode);
    if (isValidDir(selectedDir))
        return selectedDir;

    const auto fallbackMode = (mode == SamplePathMode::Step)
        ? SamplePathMode::Loop
        : SamplePathMode::Step;
    auto fallbackDir = getDefaultSampleDirectory(stripIndex, fallbackMode);
    if (isValidDir(fallbackDir))
        return fallbackDir;

    const auto currentFile = currentStripFiles[stripIndex];
    auto currentDir = currentFile.getParentDirectory();
    if (isValidDir(currentDir))
        return currentDir;

    // Cross-strip fallback so empty step strips can still browse immediately.
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (isValidDir(defaultStepDirectories[static_cast<size_t>(i)]))
            return defaultStepDirectories[static_cast<size_t>(i)];
        if (isValidDir(defaultLoopDirectories[static_cast<size_t>(i)]))
            return defaultLoopDirectories[static_cast<size_t>(i)];

        const auto otherCurrentDir = currentStripFiles[i].getParentDirectory();
        if (isValidDir(otherCurrentDir))
            return otherCurrentDir;
    }

    for (const auto& favoriteDir : browserFavoriteDirectories)
    {
        if (isValidDir(favoriteDir))
            return favoriteDir;
    }

    if (isValidDir(lastSampleFolder))
        return lastSampleFolder;

    // Last-resort fallback: allow browsing from home even with no configured paths.
    const auto homeDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (isValidDir(homeDir))
        return homeDir;

    return {};
}

juce::File MlrVSTAudioProcessor::getBrowserFavoriteDirectory(int slot) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return {};

    return browserFavoriteDirectories[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoritePadHeld(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return browserFavoritePadHeld[static_cast<size_t>(stripIndex)][static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteSaveBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteSaveBurstUntilMs[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteMissingBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteMissingBurstUntilMs[static_cast<size_t>(slot)];
}

void MlrVSTAudioProcessor::beginBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    browserFavoritePadHeld[stripIdx][slotIdx] = true;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
    browserFavoritePadPressStartMs[stripIdx][slotIdx] = juce::Time::getMillisecondCounter();
}

void MlrVSTAudioProcessor::endBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    const bool wasHeld = browserFavoritePadHeld[stripIdx][slotIdx];
    const bool holdSaveTriggered = browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx];

    if (wasHeld && !holdSaveTriggered)
    {
        if (!recallBrowserFavoriteDirectoryForStrip(stripIndex, slot))
            browserFavoriteMissingBurstUntilMs[slotIdx] = juce::Time::getMillisecondCounter() + browserFavoriteMissingBurstDurationMs;
    }

    browserFavoritePadHeld[stripIdx][slotIdx] = false;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
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

bool MlrVSTAudioProcessor::saveBrowserFavoriteDirectoryFromStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto directory = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!directory.exists() || !directory.isDirectory())
        return false;

    browserFavoriteDirectories[static_cast<size_t>(slot)] = directory;
    savePersistentDefaultPaths();
    return true;
}

bool MlrVSTAudioProcessor::recallBrowserFavoriteDirectoryForStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto slotIdx = static_cast<size_t>(slot);
    const auto directory = browserFavoriteDirectories[slotIdx];
    if (!directory.exists() || !directory.isDirectory())
    {
        browserFavoriteDirectories[slotIdx] = juce::File();
        savePersistentDefaultPaths();
        return false;
    }

    const auto mode = getSamplePathModeForStrip(stripIndex);
    setDefaultSampleDirectory(stripIndex, mode, directory);
    lastSampleFolder = directory;
    return true;
}

bool MlrVSTAudioProcessor::isAudioFileSupported(const juce::File& file) const
{
    if (!file.existsAsFile())
        return false;

    return file.hasFileExtension(".wav")
        || file.hasFileExtension(".aif")
        || file.hasFileExtension(".aiff")
        || file.hasFileExtension(".mp3")
        || file.hasFileExtension(".ogg")
        || file.hasFileExtension(".flac");
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

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        paths.setProperty(key, browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName(), nullptr);
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

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        juce::File favoriteDir(paths.getProperty(key).toString());
        if (favoriteDir.exists() && favoriteDir.isDirectory())
            browserFavoriteDirectories[static_cast<size_t>(slot)] = favoriteDir;
        else
            browserFavoriteDirectories[static_cast<size_t>(slot)] = juce::File();
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
        ControlMode::Preset,
        ControlMode::StepEdit
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

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        juce::File favoriteDir(xml->getStringAttribute("favoriteDir" + juce::String(slot)));
        if (favoriteDir.exists() && favoriteDir.isDirectory())
            browserFavoriteDirectories[static_cast<size_t>(slot)] = favoriteDir;
        else
            browserFavoriteDirectories[static_cast<size_t>(slot)] = juce::File();
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

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
        xml.setAttribute("favoriteDir" + juce::String(slot), browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName());

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

    // Apply trigger-fade setting immediately for Monome row presses, even if
    // the host isn't currently invoking processBlock.
    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);
    
    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;

    // If bar length was changed while playing, apply it on the next row trigger.
    const auto stripIdx = static_cast<size_t>(stripIndex);
    if (pendingBarLengthApply[stripIdx] && strip->hasAudio())
    {
        const int bars = juce::jlimit(1, 8, strip->getRecordingBars());
        strip->setBeatsPerLoop(static_cast<float>(bars * 4));
        pendingBarLengthApply[stripIdx] = false;
    }

    // CHECK: If inner loop is active, clear it and return to full loop
    if (strip->getLoopStart() != 0 || strip->getLoopEnd() != MaxColumns)
    {
        const int targetColumn = juce::jlimit(0, MaxColumns - 1, column);
        bool updatedPendingClear = false;
        {
            const juce::ScopedLock lock(pendingLoopChangeLock);
            auto& pending = pendingLoopChanges[static_cast<size_t>(stripIndex)];
            if (pending.active && pending.clear)
            {
                // Keep a single quantized clear request active, but allow the
                // user's latest pad press to define the post-exit position.
                pending.markerColumn = targetColumn;
                pending.postClearTriggerArmed = false;
                updatedPendingClear = true;
            }
        }

        if (updatedPendingClear)
        {
            DBG("Inner loop clear pending on strip " << stripIndex
                << " -> updated marker column " << targetColumn);
            return;
        }

        // Inner loop is active: this press both clears the loop and defines
        // the re-entry column, applied together on the quantized boundary.
        queueLoopChange(stripIndex, true, 0, MaxColumns, false, targetColumn);
        DBG("Inner loop clear+retrigger requested on strip " << stripIndex
            << " -> column " << targetColumn << " (quantized)");
        return;
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
    if (kEnableTriggerDebugLogging)
    {
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
    }
    
    // Strict gate behavior: ignore extra presses while quantized trigger is pending.
    if (useQuantize && gateClosed)
    {
        updateMonomeLEDs();
        return;
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
        // Immediate trigger - handle group choke here with short fade in engine path.
        audioEngine->enforceGroupExclusivity(stripIndex, false);
        
        // Trigger immediately with PPQ sync
        int64_t triggerGlobalSample = audioEngine->getGlobalSampleCount();
        
        strip->triggerAtSample(column, audioEngine->getCurrentTempo(), triggerGlobalSample, posInfo);
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
    applyCompletedPresetSaves();

    const int pendingPreset = pendingPresetLoadIndex.load(std::memory_order_acquire);
    if (pendingPreset >= 0)
    {
        double hostPpqSnapshot = 0.0;
        double hostTempoSnapshot = 0.0;
        if (getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot))
        {
            pendingPresetLoadIndex.store(-1, std::memory_order_release);
            performPresetLoad(pendingPreset, hostPpqSnapshot, hostTempoSnapshot);
        }
    }

    // Update monome LEDs regularly for smooth playhead
    if (monomeConnection.isConnected() && audioEngine)
    {
        const auto nowMs = juce::Time::currentTimeMillis();
        if (monomeConnection.supportsGrid()
            && (lastGridLedUpdateTimeMs == 0 || (nowMs - lastGridLedUpdateTimeMs) >= kGridRefreshMs))
        {
            updateMonomeLEDs();
            lastGridLedUpdateTimeMs = nowMs;
        }
        if (monomeConnection.supportsArc())
            updateMonomeArcRings();
    }
}

void MlrVSTAudioProcessor::loadAdjacentFile(int stripIndex, int direction)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    
    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;
    
    // Get current file for this strip.
    // If strip has no loaded audio, force first-file fallback regardless of any
    // stale path cached in currentStripFiles.
    juce::File currentFile = strip->hasAudio() ? currentStripFiles[stripIndex] : juce::File();

    // Determine folder to browse from strip-specific browser path context.
    juce::File folderToUse = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!folderToUse.exists() || !folderToUse.isDirectory())
        return;
    
    // Get all audio files in folder
    juce::Array<juce::File> audioFiles;
    for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, false))
    {
        if (isAudioFileSupported(file))
        {
            audioFiles.add(file);
        }
    }

    // If no files at top level, allow browsing into nested pack folders.
    if (audioFiles.size() == 0)
    {
        for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, true))
        {
            if (isAudioFileSupported(file))
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

    juce::File fileToLoad;
    if (currentIndex < 0)
    {
        // Requirement: if no sample is currently loaded on this strip,
        // both Prev and Next should load the first file in the selected folder.
        fileToLoad = audioFiles[0];
    }
    else
    {
        // Calculate new index with wraparound
        int newIndex = currentIndex + direction;
        if (newIndex < 0) newIndex = audioFiles.size() - 1;
        if (newIndex >= audioFiles.size()) newIndex = 0;
        fileToLoad = audioFiles[newIndex];
    }
    
    if (!fileToLoad.existsAsFile())
    {
        return;
    }
    
    // Save playback state
    bool wasPlaying = strip->isPlaying();
    const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    // Step mode playback is host-clock driven and does not rely on the loop PPQ anchor.
    // Do not block browse-load on missing timeline anchor in this mode.
    const bool requiresTimelineAnchor = wasPlaying && !isStepMode;
    float savedSpeed = strip->getPlaybackSpeed();
    float savedVolume = strip->getVolume();
    float savedPan = strip->getPan();
    int savedGroup = strip->getGroup();
    int savedLoopStart = strip->getLoopStart();
    int savedLoopEnd = strip->getLoopEnd();
    const bool savedTimelineAnchored = strip->isPpqTimelineAnchored();
    const double savedTimelineOffsetBeats = strip->getPpqTimelineOffsetBeats();
    const int savedColumn = strip->getCurrentColumn();

    double hostPpqBeforeLoad = 0.0;
    double hostTempoBeforeLoad = 0.0;
    const int64_t globalSampleBeforeLoad = audioEngine->getGlobalSampleCount();
    if (requiresTimelineAnchor)
    {
        // Strict PPQ safety for file browsing:
        // do not load when hard PPQ resync cannot be guaranteed.
        if (!savedTimelineAnchored || !getHostSyncSnapshot(hostPpqBeforeLoad, hostTempoBeforeLoad))
        {
            DBG("File browse load skipped on strip " << stripIndex
                << ": requires anchored strip + valid host PPQ/BPM.");
            return;
        }
    }
    
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
        
        // If browsing while playing, hard-restore PPQ state with deterministic
        // host-time projection based on pre-load PPQ snapshot.
        if (requiresTimelineAnchor)
        {
            const int64_t globalSampleNow = audioEngine->getGlobalSampleCount();
            const int64_t deltaSamples = juce::jmax<int64_t>(0, globalSampleNow - globalSampleBeforeLoad);
            const double samplesPerQuarter = (60.0 / juce::jmax(1.0, hostTempoBeforeLoad)) * juce::jmax(1.0, currentSampleRate);
            const double hostPpqApply = hostPpqBeforeLoad + (static_cast<double>(deltaSamples) / juce::jmax(1.0, samplesPerQuarter));

            strip->restorePresetPpqState(true,
                                         savedTimelineAnchored,
                                         savedTimelineOffsetBeats,
                                         savedColumn,
                                         hostTempoBeforeLoad,
                                         hostPpqApply,
                                         globalSampleNow);
        }
    }
    catch (...)
    {
    }
}

//==============================================================================
// Preset Management
//==============================================================================

void MlrVSTAudioProcessor::resetRuntimePresetStateToDefaults()
{
    if (!audioEngine)
        return;

    pendingPresetLoadIndex.store(-1, std::memory_order_release);

    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (auto& pending : pendingLoopChanges)
            pending = PendingLoopChange{};
    }
    {
        const juce::ScopedLock lock(pendingBarChangeLock);
        for (auto& pending : pendingBarChanges)
            pending = PendingBarChange{};
    }
    pendingBarLengthApply.fill(false);
    momentaryScratchHoldActive = false;
    momentaryStutterHoldActive = false;
    momentaryStutterActiveDivisionButton = -1;
    momentaryStutterButtonMask.store(0, std::memory_order_release);
    momentaryStutterMacroBaselineCaptured = false;
    momentaryStutterMacroCapturePending = false;
    momentaryStutterMacroStartPpq = 0.0;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
    momentaryStutterPlaybackActive.store(0, std::memory_order_release);
    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartDivisionBeats.store(1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
    for (auto& saved : momentaryStutterSavedState)
        saved = MomentaryStutterSavedStripState{};
    pendingStutterReleaseActive.store(0, std::memory_order_release);
    pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
    audioEngine->clearMomentaryStutterStrips();

    for (int i = 0; i < MaxStrips; ++i)
    {
        currentStripFiles[i] = juce::File();

        if (auto* strip = audioEngine->getStrip(i))
        {
            strip->clearSample();
            strip->stop(true);
            strip->setLoop(0, MaxColumns);
            strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            strip->setReverse(false);
            strip->setVolume(1.0f);
            strip->setPan(0.0f);
            strip->setPlaybackSpeed(1.0f);
            strip->setBeatsPerLoop(-1.0f);
            strip->setScratchAmount(0.0f);
            strip->setTransientSliceMode(false);
            strip->setPitchShift(0.0f);
            strip->setRecordingBars(1);
            strip->setFilterFrequency(20000.0f);
            strip->setFilterResonance(0.707f);
            strip->setFilterMorph(0.0f);
            strip->setFilterAlgorithm(EnhancedAudioStrip::FilterAlgorithm::Tpt12);
            strip->setFilterEnabled(false);
            strip->setSwingAmount(0.0f);
            strip->setGateAmount(0.0f);
            strip->setGateSpeed(4.0f);
            strip->setGateEnvelope(0.5f);
            strip->setGateShape(0.5f);
            strip->setStepPatternBars(1);
            strip->setStepPage(0);
            strip->currentStep = 0;
            strip->stepPattern.fill(false);
            strip->stepSubdivisionStartVelocity.fill(1.0f);
            strip->stepSubdivisions.fill(1);
            strip->stepSubdivisionRepeatVelocity.fill(1.0f);
            strip->stepProbability.fill(1.0f);
            strip->setStepEnvelopeAttackMs(0.0f);
            strip->setStepEnvelopeDecayMs(4000.0f);
            strip->setStepEnvelopeReleaseMs(110.0f);
            strip->setGrainSizeMs(1240.0f);
            strip->setGrainDensity(0.05f);
            strip->setGrainPitch(0.0f);
            strip->setGrainPitchJitter(0.0f);
            strip->setGrainSpread(0.0f);
            strip->setGrainJitter(0.0f);
            strip->setGrainPositionJitter(0.0f);
            strip->setGrainRandomDepth(0.0f);
            strip->setGrainArpDepth(0.0f);
            strip->setGrainCloudDepth(0.0f);
            strip->setGrainEmitterDepth(0.0f);
            strip->setGrainEnvelope(0.0f);
            strip->setGrainShape(0.0f);
            strip->setGrainArpMode(0);
            strip->setGrainTempoSyncEnabled(true);
        }

        audioEngine->assignStripToGroup(i, -1);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            audioEngine->setModSequencerSlot(i, slot);
            audioEngine->setModTarget(i, ModernAudioEngine::ModTarget::None);
            audioEngine->setModBipolar(i, false);
            audioEngine->setModCurveMode(i, false);
            audioEngine->setModDepth(i, 1.0f);
            audioEngine->setModOffset(i, 0);
            audioEngine->setModLengthBars(i, 1);
            audioEngine->setModEditPage(i, 0);
            audioEngine->setModSmoothingMs(i, 0.0f);
            audioEngine->setModCurveBend(i, 0.0f);
            audioEngine->setModCurveShape(i, ModernAudioEngine::ModCurveShape::Linear);
            audioEngine->setModPitchScaleQuantize(i, false);
            audioEngine->setModPitchScale(i, ModernAudioEngine::PitchScale::Chromatic);
            for (int s = 0; s < ModernAudioEngine::ModTotalSteps; ++s)
                audioEngine->setModStepValueAbsolute(i, s, 0.0f);
        }
        audioEngine->setModSequencerSlot(i, 0);

        if (auto* param = parameters.getParameter("stripVolume" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripPan" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripSpeed" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripPitch" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
    }

    for (int i = 0; i < ModernAudioEngine::MaxGroups; ++i)
    {
        if (auto* group = audioEngine->getGroup(i))
        {
            group->setVolume(1.0f);
            group->setMuted(false);
        }
    }

    for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
        audioEngine->clearPattern(i);
}

bool MlrVSTAudioProcessor::getHostSyncSnapshot(double& outPpq, double& outTempo) const
{
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (position->getPpqPosition().hasValue()
                && position->getBpm().hasValue()
                && std::isfinite(*position->getPpqPosition())
                && std::isfinite(*position->getBpm())
                && *position->getBpm() > 0.0)
            {
                outPpq = *position->getPpqPosition();
                outTempo = *position->getBpm();
                return true;
            }
        }
    }

    return false;
}

void MlrVSTAudioProcessor::performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot)
{
    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    // Always reset to a known clean runtime state before applying preset data.
    // This guarantees no strip audio/params leak across preset transitions.
    resetRuntimePresetStateToDefaults();
    loadedPresetIndex = -1;

    if (!PresetStore::presetExists(presetIndex))
    {
        // Empty slot recall keeps the freshly reset runtime defaults and does
        // not create or mutate preset files.
        presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    // Clear stale file references; preset load repopulates file-backed strips.
    for (auto& f : currentStripFiles)
        f = juce::File();

    const bool loadSucceeded = PresetStore::loadPreset(
        presetIndex,
        MaxStrips,
        audioEngine.get(),
        parameters,
        [this](int stripIndex, const juce::File& sampleFile)
        {
            return loadSampleToStrip(stripIndex, sampleFile);
        },
        hostPpqSnapshot,
        hostTempoSnapshot);

    if (loadSucceeded && PresetStore::presetExists(presetIndex))
        loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::runPresetSaveRequest(const PresetSaveRequest& request)
{
    if (!audioEngine || request.presetIndex < 0 || request.presetIndex >= MaxPresetSlots)
        return false;

    try
    {
        struct ScopedSuspendProcessing
        {
            explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
            ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
            MlrVSTAudioProcessor& processor;
        } scopedSuspend(*this);

        return PresetStore::savePreset(request.presetIndex,
                                       MaxStrips,
                                       audioEngine.get(),
                                       parameters,
                                       request.stripFiles.data());
    }
    catch (const std::exception& e)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": unknown");
        return false;
    }
}

void MlrVSTAudioProcessor::pushPresetSaveResult(const PresetSaveResult& result)
{
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        presetSaveResults.push_back(result);
    }
    presetSaveJobsInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::applyCompletedPresetSaves()
{
    std::vector<PresetSaveResult> completed;
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        if (presetSaveResults.empty())
            return;
        completed.swap(presetSaveResults);
    }

    uint32_t successfulSaves = 0;
    for (const auto& result : completed)
    {
        if (!result.success)
        {
            DBG("Preset save failed for slot " << result.presetIndex);
            continue;
        }

        loadedPresetIndex = result.presetIndex;
        ++successfulSaves;
    }

    if (successfulSaves > 0)
        presetRefreshToken.fetch_add(successfulSaves, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::savePreset(int presetIndex)
{
    if (!audioEngine || presetIndex < 0 || presetIndex >= MaxPresetSlots)
        return;

    if (!isTimerRunning())
        startTimer(kGridRefreshMs);

    PresetSaveRequest request;
    request.presetIndex = presetIndex;
    for (int i = 0; i < MaxStrips; ++i)
        request.stripFiles[static_cast<size_t>(i)] = currentStripFiles[i];

    auto* job = new PresetSaveJob(*this, std::move(request));
    presetSaveJobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    presetSaveThreadPool.addJob(job, true);

    // Keep UI/LED state responsive immediately; completion still updates token.
    loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::loadPreset(int presetIndex)
{
    try
    {
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = std::numeric_limits<double>::quiet_NaN();
        const bool hasHostSync = getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot);
        if (!hasHostSync)
        {
            DBG("Preset " << (presetIndex + 1)
                << " loaded without host PPQ/BPM snapshot; recalling audio/parameters only.");
        }

        pendingPresetLoadIndex.store(-1, std::memory_order_release);
        performPresetLoad(presetIndex, hostPpqSnapshot, hostTempoSnapshot);
    }
    catch (const std::exception& e)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": " << e.what());
    }
    catch (...)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": unknown");
    }
}

bool MlrVSTAudioProcessor::deletePreset(int presetIndex)
{
    try
    {
        const bool deleted = PresetStore::deletePreset(presetIndex);
        if (deleted)
        {
            struct ScopedSuspendProcessing
            {
                explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
                ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
                MlrVSTAudioProcessor& processor;
            } scopedSuspend(*this);

            // Deleting any preset slot should leave runtime in a clean state.
            resetRuntimePresetStateToDefaults();
            loadedPresetIndex = -1;
            updateMonomeLEDs();
        }
        if (deleted)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return deleted;
    }
    catch (...)
    {
        return false;
    }
}

juce::String MlrVSTAudioProcessor::getPresetName(int presetIndex) const
{
    return PresetStore::getPresetName(presetIndex);
}

bool MlrVSTAudioProcessor::setPresetName(int presetIndex, const juce::String& name)
{
    try
    {
        const bool ok = PresetStore::setPresetName(presetIndex, name);
        if (ok)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }
    catch (...)
    {
        return false;
    }
}

bool MlrVSTAudioProcessor::presetExists(int presetIndex) const
{
    try
    {
        return PresetStore::presetExists(presetIndex);
    }
    catch (...)
    {
        return false;
    }
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
