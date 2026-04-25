# Scene Performance Recorder Spec

## Goal

In Scene Mode, stop recording monome performance data into the pattern recorders.
Instead, each scene owns one fixed-length performance timeline.

That timeline records scene-mode monome actions such as:

- Loop strip trigger on/off
- Sample-mode trigger on/off
- Monome level-style controls such as volume, pan, speed, swing, grain, filter, delay
- Later: arc gestures, stutter gestures, scene-safe modulation gestures

The recorder is tied to the scene's own content length, not to the pattern recorder groups.
In Scene Mode there is one scene recorder button, not four pattern recorder buttons.


## Why This Is Cleaner Than Reusing PatternRecorder

Current architecture splits responsibilities like this:

- `PatternRecorder` is the only timed event container:
  [AudioEngine.h](/Users/e2_18/Downloads/mlrVST-modern/Source/AudioEngine.h)
- Monome control-page events write into active patterns:
  [PluginProcessor.cpp](/Users/e2_18/Downloads/mlrVST-modern/Source/PluginProcessor.cpp)
- Scene state mostly owns timing/recall metadata, not recorded performance content:
  [SceneScheduler.cpp](/Users/e2_18/Downloads/mlrVST-modern/Source/SceneScheduler.cpp)
- The current timeline UI is pattern-centric:
  [PluginEditor.cpp](/Users/e2_18/Downloads/mlrVST-modern/Source/PluginEditor.cpp)

That makes scene mode awkward because:

- patterns are group-based
- scenes are slot-based
- scenes already have length semantics
- scene chaining wants a deterministic scene-owned playhead

So the right abstraction is a scene-owned recorder, not a scene-flavored pattern.


## Product Decisions

### 1. Scene recorder owns one cycle length

When recording starts, freeze the scene content length from:

- `SceneScheduler::getResolvedSceneLengthBeats(...)`

Use that resolved scene length as the recorder loop length.

Do not use `repeatCount` as the event loop length.
`repeatCount` remains scheduler-level handoff behavior.

This gives:

- clean looping scene content
- deterministic editing
- no ambiguity between "what is recorded" and "how long the scene stays active"

### 2. Record semantic values, not raw monome columns

Do not store "x=11 on volume page".
Store "strip 3 volume = 0.72".

This makes:

- playback independent from monome layout changes
- editing much easier
- future GUI editing possible without monome emulation logic

### 3. Scene mode gets one recorder transport

In Scene Mode:

- one recorder button for the active scene
- optional overdub on double tap/click
- pattern recorder buttons are hidden or repurposed

Out of Scene Mode:

- existing pattern recorder workflow stays as-is

### 4. Scene recorder only captures scene-safe events in v1

Capture in v1:

- strip triggers
- strip releases
- speed
- pan
- volume
- grain page values
- filter values
- delay values
- swing/gate values if already represented as stable strip control values

Do not capture in v1:

- file browser actions
- preset actions
- group assignment
- scene launch presses
- destructive mode changes


## Data Model

Add a new scene-owned data model instead of extending `PatternRecorder`.

### New files

- `Source/ScenePerformanceRecorder.h`
- `Source/ScenePerformanceRecorder.cpp`

### Core types

```cpp
enum class ScenePerformanceEventType
{
    TriggerOn = 0,
    TriggerOff,
    ControlPoint
};

enum class ScenePerformanceControlTarget
{
    None = 0,
    Volume,
    Pan,
    Speed,
    Swing,
    GrainSize,
    GrainDensity,
    FilterEnabled,
    FilterCutoff,
    FilterResonance,
    FilterMorph,
    FilterType,
    DelayMix,
    DelayTime,
    DelayFeedback,
    DelayLowCut,
    DelayHighCut,
    DelayMode
};

struct ScenePerformanceEvent
{
    ScenePerformanceEventType type = ScenePerformanceEventType::TriggerOn;
    int stripIndex = -1;
    int column = -1;
    int sampleSliceId = -1;
    int64_t sampleStartSample = -1;
    ScenePerformanceControlTarget controlTarget = ScenePerformanceControlTarget::None;
    float value = 0.0f;
    double timeBeats = 0.0;
};

struct ScenePerformanceClip
{
    int sceneSlot = -1;
    double lengthBeats = 4.0;
    std::vector<ScenePerformanceEvent> events;
    uint32_t version = 1;
};
```

### Recorder behavior

`ScenePerformanceRecorder` should support:

- `armRecording(sceneSlot, lengthBeats, currentBeat, sceneStartBeat)`
- `startOverdub(currentBeat)`
- `stopRecording()`
- `clear(sceneSlot)`
- `recordTriggerEvent(...)`
- `recordControlPoint(...)`
- `processEventsForBeatWindow(...)`
- `getEventsSnapshot(sceneSlot)`
- `getPlaybackProgressForBeat(...)`

### Important difference from PatternRecorder

`PatternRecorder` records into one recorder instance per pattern.

`ScenePerformanceRecorder` should store one clip per scene slot:

```cpp
std::array<ScenePerformanceClip, MlrVSTAudioProcessor::SceneSlots> sceneClips;
```

Only one scene clip needs to be armed/recording at a time.


## Playback Model

Scene playback should be anchored to the active scene's start, not to an independent pattern transport.

Use these existing scene timing anchors:

- `activeSceneStartPpq`
- `sceneSequenceStartPpqValid`
- `getResolvedSceneLengthBeats(...)`

Playback should work like this:

1. Scene recall resolves the active scene slot and scene start beat.
2. Scene recorder transport uses that scene start beat as loop origin.
3. Events fire inside `[previousBeat, currentBeat)` windows.
4. On wrap, playback loops within `clip.lengthBeats`.

This keeps scene content aligned with scene-end handoff logic.


## Capture Path

### Trigger capture

Today trigger events end up in pattern recorders from the strip trigger path in:

- [PluginProcessor.cpp](/Users/e2_18/Downloads/mlrVST-modern/Source/PluginProcessor.cpp)

Add a sibling path:

- `recordSceneTriggerEvent(...)`

When `isSceneModeEnabled()` is true:

- do not write trigger events into `PatternRecorder`
- write them into the active scene clip instead

### Monome control capture

Today monome control-page moves write through:

- `recordMonomeControlPatternEvent(...)`

Add:

- `recordMonomeSceneControlEvent(...)`

When Scene Mode is active:

- branch to the scene recorder
- resolve semantic values before recording

Examples:

- volume page -> record `ScenePerformanceControlTarget::Volume` + actual normalized volume
- filter page cutoff -> record Hz-normalized target value or plain strip parameter value
- delay mode -> record actual mode enum as float/int payload

### Value thinning

For continuous controls, do not store every grid press if it repeats the same resolved value.

Add capture thinning rules:

- ignore duplicate adjacent values for same target within a small beat window
- optionally collapse dense runs to last-value-per-subdivision

This keeps scene clips editable.


## File Touchpoints

### New core files

- `Source/ScenePerformanceRecorder.h`
- `Source/ScenePerformanceRecorder.cpp`

### Audio / processor

- `Source/PluginProcessor.h`
  - own the new scene recorder
  - add active scene recording state
  - add scene record/playback helpers

- `Source/PluginProcessor.cpp`
  - branch trigger recording to scene recorder in scene mode
  - branch monome control recording to scene recorder in scene mode
  - add scene recorder transport processing in timer/audio-driven update points
  - add serialization helpers

- `Source/AudioEngine.h`
- `Source/AudioEngine.cpp`
  - keep `PatternRecorder` unchanged for non-scene mode
  - optionally host playback callback plumbing similar to `PatternControlEventCallback`

### Scene system

- `Source/SceneScheduler.h`
- `Source/SceneScheduler.cpp`
  - provide stable scene playhead origin
  - expose a helper for "content length beats at record start"
  - start/stop active scene performance playback on recall/handoff

### Monome

- `Source/MonomeController.cpp`
  - route monome top-row recorder behavior to scene recorder in scene mode
  - keep pattern-button behavior outside scene mode

### UI

- `Source/PluginEditor.h`
- `Source/PluginEditor.cpp`
  - add `ScenePerformancePanel`
  - scene timeline drawing and editing

- `Source/PluginEditorPanels.cpp`
  - add one scene recorder button to the Scene panel
  - show active scene clip state and length

### Persistence

- `Source/SceneScheduler.cpp`
  - stop using `SceneChainState` as timing-only if scene performance is added there
  - better: create a sibling `ScenePerformanceState`


## Persistence Design

Do not overload `SceneChainState` with event lists.
Keep timing metadata and performance content separate.

### Recommended XML shape

```xml
<SceneAuxState>
  <SceneChainState ... />
  <ScenePerformanceState version="1">
    <SceneClip slot="0" lengthBeats="16.0">
      <Event type="TriggerOn" strip="2" column="6" timeBeats="0.0" />
      <Event type="ControlPoint" strip="2" target="Volume" value="0.71" timeBeats="1.5" />
    </SceneClip>
  </ScenePerformanceState>
</SceneAuxState>
```

Why:

- `SceneChainState` remains small and readable
- performance data gets its own versioning
- migration is safer

### Save/load helpers to add

- `createScenePerformanceStateXml(sceneSlotOverride)`
- `applyScenePerformanceStateXml(xml, sceneSlotOverride)`

Then wrap both in a scene aux root when saving scene presets.


## UI Spec

### Main scene recorder UI

Add a new `ScenePerformancePanel`.

In Scene Mode:

- replace the current pattern-recorder panel with the scene recorder panel
- show exactly one recorder transport for the active scene

Panel content:

- scene slot label
- frozen content length
- `Rec` button
- `Play` button
- `Stop` button
- `Clear` button
- event counts by type
- editable timeline

### Timeline layout

Use a lane model, not a single mixed strip.

Recommended lanes:

- one trigger lane per strip
- one automation lane bundle per strip
- sub-lanes for `Volume`, `Pan`, `Speed`, `Filter`, `Delay`, `Swing`

### Visual language

- trigger notes: short rectangular markers
- trigger-offs: hollow markers
- continuous controls: breakpoint lines with draggable points
- current playhead: vertical line
- loop boundary and beat grid: vertical guides

### Editing tools

V1 editable operations:

- click event to select
- delete selected event
- drag trigger event horizontally
- drag automation point horizontally/vertically
- shift-drag selection to move many events
- context menu: clear lane, clear strip, quantize selection

V2 editing operations:

- marquee selection
- cut/copy/paste by beat region
- scale selection to new scene length
- simplify automation
- mute lane


## Monome UX In Scene Mode

### GUI

One scene recorder button in the Scene panel.

### Monome

In Scene Mode, the pattern-recorder area should no longer represent four pattern recorders.

Recommended top-row mapping:

- `x=4`: scene record / overdub
- `x=5`: scene play toggle
- `x=6`: stop scene performance playback
- `x=7`: clear current scene performance clip

This preserves the muscle-memory location while making the scene recorder a single transport.


## Improvements Over The Original Idea

### Improvement 1: Scene content length vs scene handoff length

Use resolved scene content length as the clip loop.
Do not bind recorded data to repeat count.

This avoids giant clips when a scene is configured to repeat many times.

### Improvement 2: Semantic automation lanes

Store final parameter values instead of monome page coordinates.
That makes editing and future GUI automation much easier.

### Improvement 3: Read-only first, editable second

The fastest safe path is:

1. scene clip data model
2. capture + playback
3. read-only timeline
4. editable timeline

That avoids building a complex editor before the event model is stable.

### Improvement 4: Keep patterns outside scene mode

Do not delete or merge the existing pattern system.
Just stop using it as the scene recorder backend.


## Staged Patch Order

### Stage 1: Scene recorder core

Create:

- `ScenePerformanceRecorder.h/.cpp`

Patch:

- `PluginProcessor.h`
- `PluginProcessor.cpp`

Deliver:

- one clip per scene slot
- record/stop/clear
- XML round-trip without playback yet

### Stage 2: Trigger capture and playback

Patch:

- `PluginProcessor.cpp`
- `SceneScheduler.cpp`

Deliver:

- strip trigger events record into active scene
- active scene playback fires trigger events on loop

### Stage 3: Control-page semantic capture

Patch:

- `MonomeController.cpp`
- `PluginProcessor.cpp`

Deliver:

- monome page events record semantic control values into scene clips
- playback reuses existing live strip-control setters

### Stage 4: Scene panel transport

Patch:

- `PluginEditor.h`
- `PluginEditor.cpp`
- `PluginEditorPanels.cpp`

Deliver:

- one scene recorder button in scene mode
- scene clip status and basic counters

### Stage 5: Read-only scene timeline

Patch:

- `PluginEditor.h`
- `PluginEditor.cpp`

Deliver:

- multi-lane visualization
- trigger markers
- automation lines
- playhead

### Stage 6: Editing

Patch:

- `PluginEditor.h`
- `PluginEditor.cpp`

Deliver:

- select / move / delete / clear lane
- drag automation points
- quantize selection

### Stage 7: Monome top-row scene transport

Patch:

- `MonomeController.cpp`
- `updateMonomeLEDs()` paths

Deliver:

- top-row scene record/play/stop/clear
- no scene-mode pattern-recorder semantics


## Recommended MVP

If we want the tightest first implementation, the MVP should be:

1. Scene clip data model
2. One scene recorder button
3. Trigger capture
4. Volume/filter/delay capture
5. Scene playback
6. Read-only timeline

Leave full editing and every page type for the next pass.


## Non-Goals For First Pass

- GUI slider recording
- file browser recording
- group recording
- preset action recording
- arbitrary editor automation outside monome scene capture


## Summary

The best implementation is not "pattern recorders, but for scenes."

It is:

- a scene-owned semantic event timeline
- fixed to the resolved scene content length
- driven by scene scheduler timing
- shown in a lane-based editor
- controlled by one recorder transport in Scene Mode

That gives scene mode a single mental model:

"A scene is both a state snapshot and a loopable performance clip."
