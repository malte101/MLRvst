# mlrVST v180 - Modern UI Redesign (Reference Image Match)

## Design Analysis

Based on the reference image, here are the key design elements to implement:

### Color Palette
```cpp
// Strip Colors (rainbow progression)
Strip 1: #ff3366 (Red)
Strip 2: #ff8833 (Orange) 
Strip 3: #ffcc33 (Yellow)
Strip 4: #66ff66 (Green)
Strip 5: #33ccff (Cyan)
Strip 6: #6666ff (Blue)
Strip 7: #cc66ff (Purple)

// Background
Main BG: #0a0a0a (very dark)
Panel BG: #1a1a1a (darker panels)
Border: Strip-specific color with glow

// Text
Primary: #ffffff
Secondary: #888888
Placeholder: #444444
```

### Key Changes to Implement

#### 1. Strip Visual Design
```cpp
// Current: Plain gray boxes
// Target: Colored borders with glow, darker background

void StripControl::paint(Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Very dark background
    g.setColour(Colour(0xff0a0a0a));
    g.fillRoundedRectangle(bounds, 12.0f);
    
    // Colored border with glow
    g.setColour(stripColor.withAlpha(0.2f));
    g.fillRoundedRectangle(bounds.expanded(2), 14.0f);
    
    g.setColour(stripColor);
    g.drawRoundedRectangle(bounds, 12.0f, 2.0f);
}
```

#### 2. Knobs (Rounded Rectangle Style)
```cpp
// Current: Circular knobs
// Target: Rounded rectangle knobs with colored ring

class ModernKnobLookAndFeel : public LookAndFeel_V4
{
    void drawRotarySlider(...)
    {
        auto bounds = Rectangle<float>(x, y, width, height);
        auto size = jmin(width, height) * 0.7f;
        auto rect = bounds.withSizeKeepingCentre(size, size);
        
        // Dark background
        g.setColour(Colour(0xff1a1a1a));
        g.fillRoundedRectangle(rect, size * 0.15f);
        
        // Colored ring (progress)
        g.setColour(stripColor);
        Path arc;
        arc.addCentredArc(rect.getCentreX(), rect.getCentreY(),
                         size * 0.5f, size * 0.5f,
                         0.0f, startAngle, angle, true);
        g.strokePath(arc, PathStrokeType(3.0f));
        
        // Center dot indicator
        float dotX = rect.getCentreX() + cos(angle) * size * 0.3f;
        float dotY = rect.getCentreY() + sin(angle) * size * 0.3f;
        g.fillEllipse(dotX - 3, dotY - 3, 6, 6);
    }
};
```

#### 3. Dropdowns (Modern Style)
```cpp
// Current: Basic combobox
// Target: Dark dropdown with rounded corners

ComboBox styling:
- Background: #1a1a1a
- Border: #333333
- Hover: #252525
- Text: White
- Arrow: Strip color
- Rounded corners: 6px
```

#### 4. Waveform Display
```cpp
// Current: Cyan waveform
// Target: Gradient waveform matching strip color

void WaveformDisplay::paint(Graphics& g)
{
    // Dark background
    g.fillAll(Colour(0xff0a0a0a));
    
    if (hasAudio)
    {
        // Gradient waveform
        ColourGradient gradient(
            stripColor.darker(0.3f), 0, height,
            stripColor, 0, 0,
            false
        );
        g.setGradientFill(gradient);
        
        // Draw waveform path
        g.strokePath(waveformPath, PathStrokeType(1.5f));
    }
    else
    {
        // "No Sample" text
        g.setColour(Colour(0xff444444));
        g.setFont(Font(14.0f));
        g.drawText("No Sample", bounds, Justification::centred);
    }
}
```

#### 5. Global Controls Layout
```cpp
// Current layout (from image):
Master | Input | L R | Tempo | Crossfade | Quantize | Quality

// Vertical sliders with:
- No text boxes
- Colored thumb (cyan/teal)
- Value on hover only
- Clean minimal design

// Rotary knobs:
- Tempo: Shows "120.0 BPM" below
- Crossfade: Shows "3.10 ms" below
- Modern rounded rectangle style
```

#### 6. Strip Colors Assignment
```cpp
const Colour getStripColor(int stripIndex)
{
    const Colour colors[] = {
        Colour(0xffff3366), // Red
        Colour(0xffff8833), // Orange
        Colour(0xffffcc33), // Yellow
        Colour(0xff66ff66), // Green
        Colour(0xff33ccff), // Cyan
        Colour(0xff6666ff), // Blue
        Colour(0xffcc66ff)  // Purple
    };
    return colors[stripIndex % 7];
}
```

#### 7. Typography
```cpp
// Strip labels: 11px bold, uppercase
stripLabel.setFont(Font(11.0f, Font::bold));
stripLabel.setText("S" + String(stripIndex + 1), dontSendNotification);

// Control labels: 9px regular, gray
controlLabel.setFont(Font(9.0f));
controlLabel.setColour(Label::textColourId, Colour(0xff888888));

// Value displays: 10px mono
valueLabel.setFont(Font("Courier New", 10.0f, Font::plain));
```

## Implementation Priority

### Phase 1: Colors & Borders (30 min)
1. Add strip color array
2. Update strip borders with color + glow
3. Darker backgrounds (#0a0a0a)
4. Color-coded knobs

### Phase 2: Modern Knobs (45 min)
1. Create ModernKnobLookAndFeel
2. Rounded rectangle style
3. Colored ring progress indicator
4. Center dot pointer

### Phase 3: Waveforms (20 min)
1. Gradient fills matching strip color
2. "No Sample" placeholder
3. Playhead indicator

### Phase 4: Dropdowns & Polish (30 min)
1. Modern dropdown styling
2. Hover states
3. Typography updates
4. Value displays

## Code Changes Summary

### StripControl.h
- Add: `juce::Colour stripColor;`
- Add: `ModernKnobLookAndFeel` class

### StripControl.cpp Constructor
```cpp
StripControl::StripControl(int idx, Processor& p)
    : stripIndex(idx)
{
    // Assign strip color
    stripColor = getStripColor(idx);
    
    // Apply to knob look and feel
    knobLookAndFeel.setStripColor(stripColor);
    
    volumeSlider.setLookAndFeel(&knobLookAndFeel);
    panSlider.setLookAndFeel(&knobLookAndFeel);
    speedSlider.setLookAndFeel(&knobLookAndFeel);
    scratchSlider.setLookAndFeel(&knobLookAndFeel);
}
```

### StripControl::paint()
```cpp
void StripControl::paint(Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Glow effect
    g.setColour(stripColor.withAlpha(0.15f));
    for (int i = 3; i > 0; --i)
    {
        g.drawRoundedRectangle(bounds.expanded(i), 12.0f + i, 1.0f);
    }
    
    // Dark background
    g.setColour(Colour(0xff0a0a0a));
    g.fillRoundedRectangle(bounds, 12.0f);
    
    // Colored border
    g.setColour(stripColor);
    g.drawRoundedRectangle(bounds, 12.0f, 2.0f);
}
```

### GlobalControlPanel Updates
```cpp
// Remove text boxes from sliders
masterVolumeSlider.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
inputMonitorSlider.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);

// Use cyan color for sliders
Colour controlColor(0xff33ccff); // Cyan
darkLookAndFeel.setColour(Slider::thumbColourId, controlColor);
```

## Visual Comparison

### Before (Current)
- Gray borders
- Blue accent color
- Basic knobs
- Text boxes on everything
- Lighter background

### After (Target)
- Rainbow colored borders with glow
- Strip-specific colors
- Modern rounded rectangle knobs
- Clean minimal UI
- Very dark background (#0a0a0a)
- "No Sample" placeholder
- Gradient waveforms

## Testing Checklist

- [ ] Each strip has unique color
- [ ] Borders have subtle glow
- [ ] Knobs are rounded rectangles
- [ ] Waveforms use gradient
- [ ] "No Sample" shows when empty
- [ ] Dropdowns are styled
- [ ] Typography is consistent
- [ ] Global controls use cyan
- [ ] Background is very dark
- [ ] All spacing matches reference

## Notes

This redesign closely matches the reference image while maintaining all existing functionality. The color-coded strips make it much easier to visually identify and work with different samples, and the modern knob style is more touchscreen-friendly and visually appealing.

The implementation can be done incrementally - start with colors and borders, then move to knobs and waveforms, finishing with polish and typography.
