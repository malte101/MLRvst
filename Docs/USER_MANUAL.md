# mlrVST User Manual

This manual covers day-to-day use of the current `mlrVST` build (GUI + monome workflow).

## 1. What mlrVST Is

`mlrVST` is a 6-strip sample performance instrument with:
- Loop, gate, step-sequencer, and grain playback modes
- Per-strip modulation sequencers
- Monome grid control via serialosc
- Preset save/load with deep strip state recall

## 2. GUI Overview

### Top Tabs
- `Global Controls`: master and global timing/render options
- `Presets`: save/load/delete presets and export strip recordings as WAV
- `Monome Device`: monome connection and device status
- `Paths`: default per-strip sample folders for Loop mode and Step mode

### Main Tabs
- `Play`: all strip controls and waveform/step editing
- `FX`: per-strip filter and gate shaping controls
- `Patterns`: 4 pattern recorders
- `Groups`: group volume/mute management

### Bottom Bar
- `Monome Pages`: control-page order and activation (row 7 page mapping)

## 3. Basic Workflow

1. Load sample(s) per strip with `Load` or file drag-and-drop.
2. Choose strip `Play Mode` (`One-Shot`, `Loop`, `Gate`, `Step`, `Grain`).
3. Choose `Direction` (`Normal`, `Reverse`, `Ping-Pong`, `Random`, `Rnd Walk`, `Rnd Slice`).
4. Adjust `VOL`, `PAN`, `SPEED`, and mode-specific controls.
5. Trigger from GUI cells or monome grid.
6. Save snapshots in `Presets`.

Supported audio files: `.wav`, `.aif`, `.aiff`, `.mp3`, `.ogg`, `.flac`.

## 4. Step Mode

Set strip `Play Mode` to `Step` to show the step editor.

### Step Toolbar Tools
- `Vol`: set velocity/height
- `Divide`: subdivision count
- `Ramp+`: rising velocity ramp
- `Ramp-`: falling velocity ramp
- `Prob`: step probability
- `Select`: multi-step selection editing

### Mouse/Modifier Behavior
- `Click cell (Vol tool)`: enable and set velocity from mouse Y position.
- `Drag (Vol tool)`: change velocity continuously.
- `Shift+click`: toggle step on/off.
- `Shift+drag on active step`: volume drag without color fill overlay.
- `Command+drag`: divide (subdivision edit).
- `Control+drag`: ramp up (auto-divides undivided steps).
- `Option+drag`: ramp down (auto-divides undivided steps).
- `Command+Shift`: toggle `Select` tool on/off.

If you drag volume to the bottom (near zero), the step is disabled.

### Selection Tool
- `Click step`: select/deselect that step.
- `Drag`: lasso-select range.
- Selection is remembered when toggling away from and back to `Select`.
- When selected steps are active:
  - `Command+drag` applies divide over the selection.
  - `Control+drag` applies ramp up over the selection.
  - `Option+drag` applies ramp down over the selection.

### Step Context Menu
Right-click a step (without edit modifiers) for:
- Enable/Disable
- Divide x2 / x4
- Reset step / reset selected
- Probability quick values (100/75/50/25)

## 5. Modulation Editing

Each strip has a modulation sequencer with:
- Target, depth, bipolar, smoothing
- Length in bars and edit page
- Curve/step shape controls
- Optional pitch quantization and scale

Mouse editing on modulation steps:
- Drag: draw values
- `Command+drag`: duplicate/redistribute around clicked cell
- `Control+drag`: shape up (curve up)
- `Option+drag`: shape down (curve down)
- Right-click lane: reset full lane to neutral
- Double-click step: reset one step to neutral

## 6. Monome Grid Operation

Grid rows:
- `y=0`: group/pattern/system row (depends on mode)
- `y=1..6`: strip rows (one per strip)
- `y=7`: control-page row

### Main (Normal) Page
- `y=0, x=0..3`: group mute/unmute
- `y=0, x=4..7`: pattern record/play/stop states
- `y=0, x=8`: momentary scratch hold
- `y=0, x=9..15`: momentary stutter divisions
- `y=1..6`: strip triggers (step-mode strips are editable from main page)

### Control Pages (row 7)
Row 7 buttons open control pages (order is configurable in `Monome Pages`):
- `Speed`, `Pan`, `Volume`, `Grain Size`, `Swing`, `Gate`, `Browser`,
  `Group`, `Filter`, `Pitch`, `Modulation`, `Preset`, `Step Edit`

`Global Controls > Momentary`:
- On: row 7 pages are hold-to-use
- Off: row 7 pages toggle latch state

### Step Edit Page (Monome)

Top row (`y=0`):
- `x0..x7`: `Velocity`, `Divide`, `Ramp+`, `Ramp-`, `Probability`, `Attack`, `Decay`, `Pitch`
- `x8..x13`: select strip for step editing

Strip rows (`y=1..6`):
- Edit 16 visible steps with the selected tool.
- In `Velocity`, bottom row (`y=6`) is step-off (disable).

Bottom row (`y=7`):
- `x13`: pitch -1 semitone
- `x14`: pitch +1 semitone
- `x15`: quantize toggle

### Group Page (Monome)
Per strip row:
- `x0..x4`: ungroup or assign Group 1..4
- `x6..x11`: playback direction mode
- `x13..x15`: strip mode `Loop`, `Step`, `Grain`

### Browser Page (Monome)
Per strip row:
- `x0`: previous sample
- `x1`: next sample
- `x2`: spacer (unused)
- `x3..x8`: 6 favorite-folder slots
- `x11..x14`: recording/loop bars (`1`, `2`, `4`, `8`)
- `x15`: capture recent input audio to strip

Favorites:
- Tap slot: recall favorite directory
- Hold slot ~3s: save current browser directory into slot
- Missing folder slots are safely rejected and visually flagged

If no sample is loaded on a strip, `Prev` and `Next` both load the first file in the active folder.

## 7. Presets and Persistence

### Presets
- Slots: `16 x 7 = 112`
- `Click`: load
- `Shift+Click`: save
- `Right-click`: delete
- Preset names can be edited in the Presets tab

Saved preset data includes strip playback/mode state, step data, modulation data, grain data, groups/patterns, and parameter state. If a strip has audio without a valid file path, audio may be embedded in the preset.

When loading presets, global controls (for example master, quantize, and similar global settings) stay at the current session values.

### Host Session State
Regular plugin session save/load also stores:
- Current parameter state
- Default Loop/Step path settings
- Browser favorite folders
- Monome page order and momentary mode

## 8. Tips

- Use `Paths` tab to set separate default folders for Loop mode and Step mode.
- Use `Monome Pages` to put your most-used control pages first.
- For step programming speed: stay in `Vol`, then use modifier drags (`Cmd/Ctrl/Opt`) for divide and ramps.
