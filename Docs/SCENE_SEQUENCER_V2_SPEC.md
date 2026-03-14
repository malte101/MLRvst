# Scene Sequencer V2 Spec

## Purpose

This document defines the next scene-sequencer step for `mlrVST`:

- richer scene authoring
- explicit scene length behavior
- `Fill Hold` scene performance mode
- macro-to-modulation-lane recording from the Macro page
- a shared target map for macro and modulation systems

The goal is to make scenes feel like a first-class musical performance feature inside the existing `mlrVST` workflow, not a generic clip launcher bolted onto the side.

## Current Baseline

Current scene behavior is centered on four scene slots, quantized recall, repeat counts, and monome top-row chaining.

Relevant current code:

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/PluginEditor.cpp`
- `Source/MonomeController.cpp`
- `Source/PresetStore.cpp`
- `Source/AudioEngine.h`
- `Source/AudioEngine.cpp`

Current limitations:

- scene slots behave mostly like hidden sub-presets
- there is no direct `Capture Scene` / `Insert Scene` authoring workflow
- there is no temporary `Fill Hold` behavior
- scene length is implicit instead of selectable
- macro targets and modulation targets are duplicated and can drift apart
- macro gestures cannot be recorded directly into modulation lanes

## Design Goals

- Preserve the current monome-first performance identity.
- Keep PPQ-safe scene changes deterministic.
- Make scene authoring fast enough for live improvisation.
- Keep macro and modulation behavior musically identical when they target the same parameter.
- Build the new system in phases without rewriting the entire preset format at once.

## Non-Goals For V1

- no full arranger timeline
- no freehand automation curves for macro recording in v1
- no host tempo override per scene
- no unlimited scene count in the live scene row

## Product Model

Scene V2 should separate three concepts that are currently blended together.

### 1. Scene Content

This is the reusable musical state.

It includes:

- per-strip playback state
- sample/file references
- strip mode state
- active pattern assignments
- active modulation slot and modulation lane state
- macro snapshot values
- group and mute context needed for recall

### 2. Scene Slot Definition

This is the performance-facing wrapper around content.

It includes:

- content reference
- scene name
- scene color
- repeat count
- scene behavior mode
- return quantize
- scene length mode
- manual bars
- anchor strip

### 3. Scene Runtime State

This is the temporary live state.

It includes:

- active scene slot
- active scene start PPQ
- pending quantized recall
- current scene chain
- `Fill Hold` return target
- chain timing state

## Core Types

### Shared Target Map

Replace separate `MacroTarget` and `ModTarget` enums with one shared enum and one shared registry.

Suggested enum:

```cpp
enum class PerformanceTarget
{
    None = 0,
    Volume,
    Pan,
    Pitch,
    Speed,
    Cutoff,
    Resonance,
    FilterMorph,
    SliceLength,
    Scratch,
    GrainSize,
    GrainDensity,
    GrainPitch,
    GrainPitchJitter,
    GrainSpread,
    GrainJitter,
    GrainPositionJitter,
    GrainRandom,
    GrainArp,
    GrainCloud,
    GrainEmitter,
    GrainEnvelope,
    GrainShape,
    Retrigger
};
```

Suggested registry entry:

```cpp
struct PerformanceTargetSpec
{
    PerformanceTarget target = PerformanceTarget::None;
    const char* displayName = "None";
    bool macroAllowed = false;
    bool modLaneAllowed = false;
    bool supportsBipolar = false;
    bool supportsPitchScaleQuantize = false;
    float defaultNormalizedValue = 0.0f;
    float neutralNormalizedValue = 0.0f;
};
```

Ownership:

- enum and registry accessors in a shared place, for example `Source/PerformanceTargets.h/.cpp`
- `PluginProcessor` uses it for macros
- `AudioEngine` uses it for modulation lanes
- `PluginEditor` uses it for combo population and labels

This removes duplicated target conversion logic currently spread across:

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/AudioEngine.h`
- `Source/AudioEngine.cpp`
- `Source/PluginEditor.cpp`

### Scene Length Mode

```cpp
enum class SceneLengthMode
{
    LongestStrip = 0,
    LongestPattern,
    ManualBars,
    AnchorStrip
};
```

### Scene Behavior Mode

```cpp
enum class SceneBehaviorMode
{
    Normal = 0,
    FillHold
};
```

### Scene Return Quantize

```cpp
enum class SceneReturnQuantize
{
    Immediate = 0,
    NextStep,
    NextBeat,
    NextBar,
    SceneEnd
};
```

### Scene Content State

```cpp
struct SceneContentState
{
    juce::Uuid contentId;
    std::unique_ptr<juce::XmlElement> sceneStateXml;
};
```

Notes:

- for v1, `sceneStateXml` can wrap the existing scene save/load payload
- this gives linked scenes a stable content handle without forcing a total preset rewrite

### Scene Slot State

```cpp
struct SceneSlotState
{
    juce::Uuid contentId;
    juce::String name;
    juce::Colour colour;
    int repeatCount = 1;
    SceneBehaviorMode behaviorMode = SceneBehaviorMode::Normal;
    SceneReturnQuantize returnQuantize = SceneReturnQuantize::NextBar;
    SceneLengthMode lengthMode = SceneLengthMode::LongestStrip;
    int manualBars = 4;
    int anchorStrip = 0;
};
```

### Fill Hold Runtime

```cpp
struct FillHoldReturnState
{
    bool active = false;
    int returnSceneSlot = -1;
    int returnMainPresetIndex = -1;
    double returnTargetPpq = -1.0;
    double returnTempo = 120.0;
    int64_t returnGlobalSample = -1;
    SceneReturnQuantize quantize = SceneReturnQuantize::NextBar;
};
```

### Macro Lane Record State

```cpp
struct MacroLaneRecordState
{
    bool armed = false;
    bool recording = false;
    int stripIndex = -1;
    int macroIndex = -1;
    int laneSlot = 0;
    PerformanceTarget target = PerformanceTarget::None;
    double startPpq = -1.0;
    double nextStepPpq = -1.0;
    int stepsWritten = 0;
    int totalStepsToWrite = 0;
};
```

## Ownership And File-Level Changes

### `PluginProcessor`

Primary owner of:

- scene authoring commands
- scene slot metadata
- linked vs unique duplication
- fill-hold runtime return state
- macro lane record runtime state
- project save/load of Scene V2 metadata

Additions to `Source/PluginProcessor.h`:

- `captureSceneSlotFromCurrentState(int sceneSlot)`
- `insertSceneFromCurrentState(int insertAfterSceneSlot)`
- `duplicateSceneLinked(int srcSceneSlot, int dstSceneSlot)`
- `duplicateSceneUnique(int srcSceneSlot, int dstSceneSlot)`
- `getSceneSlotState(int sceneSlot) const`
- `setSceneSlotState(int sceneSlot, const SceneSlotState&)`
- `getResolvedSceneLengthBeats(int sceneSlot) const`
- `beginMacroLaneRecording(int macroIndex, int laneSlot)`
- `cancelMacroLaneRecording(int macroIndex)`
- `updateMacroLaneRecording(const juce::AudioPlayHead::PositionInfo&, int numSamples)`

State members:

- `std::array<SceneSlotState, SceneSlots> sceneSlotStates`
- `juce::HashMap<juce::Uuid, std::unique_ptr<juce::XmlElement>> sceneContentStore`
- `FillHoldReturnState fillHoldReturnState`
- `std::array<MacroLaneRecordState, MacroCount> macroLaneRecordStates`

### `AudioEngine`

Primary owner of:

- modulation lane storage
- modulation lane target behavior
- lane write helpers for recorded macro gestures

Additions to `Source/AudioEngine.h`:

- `setModTarget(int stripIndex, PerformanceTarget target)`
- `PerformanceTarget getModTarget(int stripIndex) const`
- `writeModStepNormalized(int stripIndex, int slot, int absoluteStep, float normalizedValue)`
- `int getModTotalActiveSteps(int stripIndex, int slot) const`
- `double getModStepLengthBeats(int stripIndex, int slot) const`

Do not make macro recording write directly to UI state. It should write through engine helpers.

### `PluginEditor`

Primary owner of:

- Scene tab authoring buttons
- scene metadata widgets
- macro record-to-lane controls

Scene tab additions in `SceneControlPanel`:

- `Capture`
- `Insert Before`
- `Insert After`
- `Duplicate Linked`
- `Duplicate Unique`
- `Behavior`
- `Return`
- `Length Mode`
- `Manual Bars`
- `Anchor Strip`

Macro page additions in `MacroControlPanel`:

- small `Rec Lane` toggle/button next to each macro
- lane selector
- state lamp or status text

### `MonomeController`

Primary owner of:

- fill-hold top-row interaction
- release-to-return behavior
- LED feedback for normal scene, queued scene, fill scene, active return target

Behavior change:

- a scene slot marked `FillHold` should store the return state on press
- on release, request a quantized recall back to the stored state
- if another explicit scene launch occurs while held, cancel auto-return

## Scene Commands

### Capture Scene

Behavior:

- serializes current live scene state into a new or existing `SceneContentState`
- updates selected `SceneSlotState.contentId`
- preserves slot metadata like name, color, repeat count, and length mode

Suggested implementation:

1. call existing scene-state creation path
2. generate content ID if slot has no content yet
3. overwrite content payload in `sceneContentStore`

### Insert Scene

Behavior:

- inserts a new scene slot before or after the selected slot
- shifts later slot metadata to the right
- captures current live state into the inserted slot
- if the row is full, drop the last slot

For the current four-slot scene row, this is the most predictable behavior.

### Duplicate Linked

Behavior:

- copies slot metadata from source slot
- copies content reference only
- gives the destination slot a new name like `Scene 2 copy`

### Duplicate Unique

Behavior:

- copies slot metadata
- deep copies content payload
- assigns a new content ID

## Fill Hold Behavior

### Normal Press

- same as current quantized scene recall

### Fill Hold Press

- capture current return state
- launch the fill scene using current launch quantize rules
- mark `fillHoldReturnState.active = true`

### Fill Hold Release

- if return state is still valid, quantize recall back to stored scene
- release action uses the slot's `returnQuantize`

### Cancellation Rules

- explicit launch of another scene while held cancels return
- loading a new preset cancels return
- disabling scene mode cancels return

## Explicit Scene Length Behavior

Every scene slot gets one length mode.

### `Longest Strip`

Use the longest participating strip cycle.

Implementation:

- build on existing `computeStripSceneSequenceLengthBeats(int stripIndex) const`
- take the maximum across scene-participating strips

### `Longest Pattern`

Use the longest participating pattern recorder cycle.

Implementation:

- inspect active pattern lengths participating in the scene
- if no active patterns, fall back to `Longest Strip`

### `Manual Bars`

Use user-entered bar length only.

Implementation:

- `resolvedBeats = manualBars * beatsPerBar`

### `Anchor Strip`

Use one strip as timing anchor.

Implementation:

- resolve chosen strip length
- if strip is unavailable or empty, fall back to `Longest Strip`

### UI Requirement

The Scene tab should show:

- selected mode
- resolved bars/beats as read-only text

Example:

`Length: Anchor Strip (Strip 3) -> 8 bars`

## Macro To Mod Lane Recording

### UX Rules

- recording is transport-aware and PPQ-based
- recording starts when armed and transport is running
- recording writes to the selected strip's chosen lane
- lane target must match macro target
- recording stops automatically when the lane length is filled

### V1 Record Mode

Only support replace-mode one-pass step recording.

Do not support:

- overdub
- freehand point curves
- mixed target recording

### Record Resolution

Use current modulation lane grid:

- one write per effective lane step
- align writes to PPQ boundaries
- lane page view must not affect recording length

### Suggested Flow

1. User clicks `Rec Lane` on Macro 1.
2. User chooses lane `2`.
3. Processor resolves selected strip.
4. Processor checks target compatibility.
5. If needed, lane target is set to macro target.
6. On next valid PPQ boundary, recording starts.
7. Each grid step writes one normalized macro value.
8. After `totalStepsToWrite`, recording stops and disarms.

### Target Compatibility

Use shared target map metadata:

- if `modLaneAllowed == false`, button is disabled
- if target supports bipolar, lane bipolar default can be enabled
- if target supports pitch scale quantize, existing pitch lane options remain active

### Quantization Algorithm

Given:

- lane bars
- steps per bar
- host PPQ

Compute:

- `laneLengthBeats`
- `stepLengthBeats`
- `stepIndex = floor((currentPpq - startPpq) / stepLengthBeats)`

When `stepIndex` advances:

- sample current normalized macro value
- write value into modulation lane step `stepIndex`

### V1 Data Write Policy

- if lane is in step mode, set step value directly
- if lane is in curve mode, still write one step value per step and leave curve shape untouched

This keeps v1 simple and avoids editor-side surprises.

## Serialization

### Preserve Existing Scene XML

Do not replace the current scene payload format immediately.

Wrap it instead:

- keep existing scene state XML for actual scene content
- add a new Scene V2 metadata layer in plugin state

### Suggested ValueTree Layout

```xml
<SceneV2>
  <ContentStore>
    <Content id="{uuid}">
      <SceneState>...</SceneState>
    </Content>
  </ContentStore>
  <Slots>
    <Slot index="0"
          contentId="{uuid}"
          name="Intro"
          colour="ff3f6f"
          repeats="2"
          behavior="0"
          returnQuantize="3"
          lengthMode="0"
          manualBars="4"
          anchorStrip="0"/>
  </Slots>
</SceneV2>
```

### Macro Lane Record State

Do not persist live armed/recording runtime.

Only persist stable lane content already written into modulation sequencers.

## UI Wireframe

### Scene Tab

Top section:

- `Scene Mode`
- current scene slot selector
- scene name
- scene color

Authoring row:

- `Capture`
- `Insert Before`
- `Insert After`
- `Dup Linked`
- `Dup Unique`

Behavior row:

- `Behavior`
- `Return`

Length row:

- `Length Mode`
- `Manual Bars`
- `Anchor Strip`
- resolved length readout

Repeat row:

- keep current repeat boxes for `S1..S4`

### Macro Tab

Per macro column:

- `M1`
- MIDI learn button
- target selector
- lane selector
- `Rec Lane` button
- rotary macro knob
- tiny record state label

Suggested record-state labels:

- `Idle`
- `Armed`
- `Rec`
- `Done`
- `Err`

### Monome

Keep current top-row scene launch and drag-chain behavior.

Additions:

- fill scene pads use a distinct LED state
- held fill scene blinks or pulses
- queued return scene lights dimmer than active scene

## Suggested Implementation Phases

### Phase 1

- add shared target map
- port existing macro and modulation target code to it

### Phase 2

- add explicit scene slot metadata
- add scene length modes
- keep existing scene content save/load payload

### Phase 3

- add `Capture Scene`
- add `Insert Scene`
- add `Duplicate Linked`
- add `Duplicate Unique`

### Phase 4

- add `Fill Hold`
- add monome release-return logic

### Phase 5

- add macro-to-mod-lane recording
- start with replace-mode one-pass capture

## Risks

- linked scene content can be confusing unless the UI shows linked state clearly
- macro recording can surprise users if target retargeting happens silently
- fill-return logic can feel wrong if return quantize is too hidden
- explicit scene length must not break current PPQ-safe scene sequencing

## Open Questions

- should `Insert Scene` also exist on the monome, or GUI only?
- should `Fill Hold` be available only on dedicated scenes, or as a modifier action for any scene?
- should `Duplicate Linked` visibly badge the slot as linked?
- should macro recording always use the currently selected strip, or a pinned strip?

## External References

These references informed the product direction and were checked on March 13, 2026.

- Maschine separates experimentation and arrangement through `Ideas view` and `Song view`, and allows duplicated scenes plus unique copies.
  Source: https://www.native-instruments.com/ni-tech-manuals/maschine-software-manual/en/working-with-the-arranger
- Maschine sections support both auto length and manual length.
  Source: https://www.native-instruments.com/ni-tech-manuals/maschine-software-manual/en/working-with-the-arranger
- Maschine treats macro controls as reusable parameter aliases rather than a separate automation domain.
  Source: https://www.native-instruments.com/ni-tech-manuals/maschine-software-manual/en/audio-routing%2C-remote-control%2C-and-macro-controls
- Maschine's modern modulation editor records optimized, cleaned gesture data instead of raw stair-steps.
  Source: https://support.native-instruments.com/hc/en-us/articles/31253791328029-How-to-use-the-Modulation-Editor-in-Maschine-3-4
- Maschine Lock snapshots are a separate morph/snapshot layer, which supports keeping `Fill Hold` or future morph scenes distinct from normal scene recall.
  Source: https://www.native-instruments.com/ni-tech-manuals/maschine-mk3-manual/en/playing-on-the-controller.html
- Ableton supports `Insert Scene` and `Capture and Insert Scene`, which is a strong model for live scene authoring without interruption.
  Source: https://www.ableton.com/en/live-manual/12/session-view/
- Bitwig supports release behaviors such as `Return`, which is a strong reference for `Fill Hold`.
  Source: https://www.bitwig.com/userguide/latest/acquiring_and_working_with_launcher_clips
- Octatrack scenes are locked parameter sets designed for interpolation between states, which supports a future separate morph-scene layer.
  Source: https://www.elektron.se/wp-content/uploads/2024/09/Octatrack-MKII-User-Manual_ENG_OS1.40A_210414.pdf
