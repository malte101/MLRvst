# mlrVST v184 - Spacebar Transport Control (Standalone)

## Feature: Spacebar Stop/Start

In standalone mode, pressing **SPACEBAR** toggles transport playback:
- Playing → Stopped
- Stopped → Playing

## Usage

**In Standalone Mode:**
```
Press SPACEBAR → Transport stops
Press SPACEBAR → Transport starts
```

**In Plugin Mode (DAW):**
- Spacebar has no effect (host controls transport)
- Use your DAW's transport controls as normal

## Implementation

### Transport State Management

**PluginProcessor.h:**
```cpp
// Standalone transport control
void setStandaloneTransportPlaying(bool shouldPlay);
bool isStandaloneTransportPlaying() const;

private:
    bool standaloneTransportPlaying = true;  // Start playing by default
```

### Process Block Integration

**PluginProcessor.cpp - processBlock():**
```cpp
#if JucePlugin_Build_Standalone
    posInfo.setIsPlaying(standaloneTransportPlaying);
#endif
```

This overrides the transport state in standalone mode, while respecting the host transport in plugin mode.

### Keyboard Input Handling

**PluginEditor.h:**
```cpp
bool keyPressed(const juce::KeyPress& key) override;
```

**PluginEditor.cpp:**
```cpp
MlrVSTAudioProcessorEditor::MlrVSTAudioProcessorEditor(...)
{
    // Enable keyboard input
    setWantsKeyboardFocus(true);
    ...
}

bool MlrVSTAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    #if JucePlugin_Build_Standalone
    if (key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        // Toggle transport
        bool playing = audioProcessor.isStandaloneTransportPlaying();
        audioProcessor.setStandaloneTransportPlaying(!playing);
        return true;  // Key handled
    }
    #endif
    
    return false;  // Pass through
}
```

## How It Works

### Standalone Mode:
1. User presses SPACEBAR
2. `keyPressed()` detects spacebar
3. Toggles `standaloneTransportPlaying` flag
4. `processBlock()` uses this flag for `posInfo.setIsPlaying()`
5. Audio engine respects transport state
6. Strips stop/start based on `hostIsPlaying`

### Plugin Mode:
1. User presses SPACEBAR
2. `keyPressed()` returns false (not compiled for plugin)
3. DAW handles spacebar normally
4. Host provides transport state via `posInfo`
5. No interference with DAW transport

## Code Isolation

The feature is **completely isolated** to standalone builds:

```cpp
#if JucePlugin_Build_Standalone
    // Standalone-specific code here
#endif
```

This ensures:
- ✅ Zero overhead in plugin mode
- ✅ No conflicts with DAW keyboard shortcuts
- ✅ Clean separation of concerns

## Visual Feedback

**Currently:** No visual transport indicator (audio starts/stops)

**Future Enhancement:** Could add visual indicator showing play/stop state in the UI

## Benefits

1. **Quick control:** Start/stop without reaching for mouse
2. **Standard behavior:** Spacebar is universal transport control
3. **No conflicts:** Only active in standalone, invisible in DAW
4. **Lightweight:** Single boolean flag, minimal code

## Testing

**Standalone:**
```
1. Launch mlrVST Standalone
2. Load samples
3. Trigger strips
4. Press SPACEBAR → All audio stops
5. Press SPACEBAR → Audio resumes
```

**Plugin:**
```
1. Load mlrVST in DAW
2. Use DAW's transport controls
3. SPACEBAR works as DAW defines
4. No interference from plugin
```

## Technical Notes

### Why Override PositionInfo?

The audio engine checks `posInfo.getIsPlaying()` to determine if it should output audio:

```cpp
bool hostIsPlaying = positionInfo.getIsPlaying();
if (!hostIsPlaying)
    return;  // Don't output audio when stopped
```

By setting `posInfo.setIsPlaying()` in standalone, we control this behavior without modifying the audio engine's logic.

### Keyboard Focus

The editor needs keyboard focus to receive key events:

```cpp
setWantsKeyboardFocus(true);  // In constructor
```

This allows the main window to receive keyboard input even when child components have focus.

## Compatibility

- ✅ macOS Standalone
- ✅ Windows Standalone  
- ✅ Linux Standalone
- ✅ AU Plugin (no effect)
- ✅ VST3 Plugin (no effect)
- ✅ AAX Plugin (no effect)

## Summary

Standalone mode now supports spacebar transport control for quick stop/start without using the mouse. The implementation is clean, isolated to standalone builds, and has zero impact on plugin operation.
