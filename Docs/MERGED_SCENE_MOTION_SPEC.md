# Merged Scene + Motion Spec

## Intent

Replace the separate user-facing `Scene` and `Mod` concepts with one unified Scene system.

The merged system should:

- keep scenes as the main linear arrangement unit
- preserve `Follow Strip` motion
- avoid duplicate ownership of the same parameter
- make recording behavior obvious
- keep Group mode unchanged

This is a product and implementation spec for a full merge.

## Core Model

Every strip in a scene has:

1. `Trigger lane`
   - scene-timed trigger events
   - trigger time + trigger offset

2. `Base lanes`
   - linear scene-time parameter automation

3. `Motion lanes`
   - cyclic modulation attached to a target
   - one motion lane per target per strip

4. `Result preview`
   - read-only combined output of Base + Motion

The merged system removes the separate mod-page mental model.

## Principle

There is exactly one owner for each layer:

- Scene owns `Base`
- Motion owns `cyclic modulation`
- Audio output hears `Result = motion(base)`

That means:

- Scene recording writes `Base`
- Motion is edited directly, not silently re-recorded into Base
- baking Motion into Base is explicit

## Timing Domains

The biggest requirement is preserving `Follow Strip`.

The solution is: motion lanes carry their own timing domain.

### Motion timing modes

`MotionTimingMode`

- `SceneLoop`
  - motion runs against scene-local beat position
  - linear, global, scene-relative

- `FollowStrip`
  - motion runs against strip-local playback phase
  - retrigger resets phase
  - trigger offset changes the phase anchor automatically

These replace the current confusing `Free` / `Sync` labels.

### Scene remains linear

Scenes stay linear at the top level.

`FollowStrip` does not make the scene non-linear.
It means a motion lane inside the scene is evaluated from strip playback state instead of scene beat.

So the scene timeline stays linear, while motion lanes can be:

- scene-clocked
- strip-clocked

## Data Model

## Scene clip

```cpp
struct SceneClip
{
    SceneLength length;
    std::array<SceneStripTrack, MaxStrips> strips;
};
```

## Per-strip scene track

```cpp
struct SceneStripTrack
{
    std::vector<SceneTriggerEvent> triggers;
    std::vector<SceneBaseLane> baseLanes;
    std::vector<SceneMotionLane> motionLanes;
    StripSceneOptions options;
};
```

## Trigger event

```cpp
struct SceneTriggerEvent
{
    double timeBeats;
    int column;
    int sampleSliceId;
    int64_t sampleStartSample;
    bool noteOn;
};
```

## Base lane

```cpp
enum class SceneTarget
{
    Speed,
    Pitch,
    Pan,
    Volume,
    Swing,
    GrainSize,
    GrainDensity,
    GrainPitch,
    GrainJitter,
    GrainRandomDepth,
    GrainEnvelope,
    FilterFrequency,
    FilterResonance,
    FilterMorph,
    DelayMix,
    DelayTime,
    DelayFeedback,
    DelayLowCut,
    DelayHighCut,
    DelayMode,
    DelaySyncEnabled
};

struct SceneBasePoint
{
    double timeBeats;
    float value;
};

struct SceneBaseLane
{
    SceneTarget target;
    std::vector<SceneBasePoint> points;
};
```

## Motion lane

```cpp
enum class MotionTimingMode
{
    SceneLoop,
    FollowStrip
};

enum class MotionShapeMode
{
    Steps,
    CurvedSteps
};

struct SceneMotionLane
{
    SceneTarget target;
    MotionTimingMode timingMode;
    bool bipolar;
    float depth;
    float rate;
    int offsetSteps;
    int lengthBars;
    float smoothingMs;
    float curveBend;
    int curveShape;
    MotionShapeMode shapeMode;
    std::array<float, ModTotalSteps> steps;
    std::array<int, ModTotalSteps> stepSubdivisions;
    std::array<float, ModTotalSteps> stepEndValues;
    std::array<int, ModTotalSteps> stepCurveShapes;
};
```

## Strip scene runtime

This is not persisted as part of the clip; it is per-playback-instance state.

```cpp
struct SceneStripRuntime
{
    bool active = false;
    double sceneTriggerBeat = 0.0;
    int64_t triggerGeneration = 0;
    double stripLoopPhaseAtLastUpdate = 0.0;
    bool stripLoopPhaseValid = false;
};
```

This runtime is what allows `FollowStrip` to work cleanly.

## Lane Ownership Rule

Per strip, per target:

- exactly one Base lane
- exactly one Motion lane

That is the main simplification.

No more six unrelated mod slots all fighting for the same target in normal use.

If advanced layering is needed later, it can be added as a hidden advanced mode, but the default system should be one target = one motion lane.

## Playback Rules

## Trigger playback

Trigger lanes are scene-time events.

When a trigger fires:

- strip playback is triggered as now
- strip runtime updates:
  - `active = true`
  - `sceneTriggerBeat = eventBeat`
  - `triggerGeneration++`

That runtime anchor is used by `FollowStrip` motion.

## Base evaluation

For each target, the scene computes the held Base value at the current scene beat.

This is the same conceptual model as current scene automation:

- latest point at or before beat wins
- if no previous point exists, use the last point or the strip base default

## Motion evaluation

### SceneLoop motion

Evaluate motion phase from:

- current scene local beat
- motion rate
- motion offset
- motion length

This is linear and deterministic at scene level.

### FollowStrip motion

Evaluate motion phase from strip-local state.

Preferred source:

- strip loop phase / strip playback phase

Behavior:

- retrigger resets motion phase
- trigger offset changes strip-local phase anchor
- if strip is stopped, motion is inactive

This preserves the useful current `Sync` behavior, but with a name that matches what it really is.

## Final value composition

The result pipeline is:

1. resolve strip default / loaded parameter value
2. apply Scene Base
3. apply Motion
4. render

The composition method depends on target:

- `Volume`
  - multiplicative / scaled mix
- `Pan`, `Pitch`, `Filter`, `Delay`, `Grain`
  - additive or normalized-offset mapping
- `DelayMode`, `DelaySyncEnabled`
  - discrete stepped override

This is the same idea as the current engine, but now represented as one merged system.

## Recording Rules

## Default scene recording

When Scene record is enabled:

- record triggers into trigger lane
- record monome slider / control gestures into Base lanes
- do not record Motion lanes
- do not record the combined heard Result

This is the safest and most intuitive default.

Reason:

- recording heard Result while Motion remains active would double-apply modulation on playback
- recording Motion implicitly makes ownership unclear

## Optional explicit recording modes

Advanced options can exist, but not as the default:

- `Record Base`
- `Record Motion`
- `Record Result To Base`

Recommended default:

- `Record Base`

## Baking Rules

Add explicit destructive actions:

- `Bake Motion Into Base`
- `Bake Selected Motion Lane Into Base`
- `Suspend Motion During Record`
- `Clear Motion`

These commands solve the "I want what I hear committed into the scene" use case without making standard recording ambiguous.

## UI Model

## Scene strip layout

Each strip section becomes:

1. trigger lane
2. target rows

Each target row includes:

- Base lane drawing
- Motion overlay
- Result overlay
- lane chips

Example chips:

- `BASE`
- `MOTION`
- `SceneLoop`
- `FollowStrip`
- `Bake`
- `Mute Motion`

## How FollowStrip is drawn

`FollowStrip` should not be drawn as a fake long linear automation curve.

That is misleading.

Instead:

- draw it as a local repeating mini-pattern overlay
- or a compact motion badge on the lane
- optionally show a ghosted preview anchored to the currently selected trigger

The visual language should clearly say:

- this motion is local to strip playback
- not global scene time

## Result overlay

Always show the effective result as a read-only brighter line when:

- Motion is active
- Base exists

This removes ambiguity and lets the user see what they will hear.

## Lane editing

Base lane editing:

- draw
- erase
- move
- quantize
- scale

Motion lane editing:

- edit steps
- edit subdivisions
- edit shape
- edit depth
- edit rate
- edit timing mode
- edit length
- edit offset

The user edits both from Scene Mode.
There is no need to leave to a separate Mod page.

## Special Cases

## Discrete targets

Some current targets do not fit nicely as normal Base + Motion lanes:

- `Retrigger`
- `Rearrange`
- `FilterEnable`
- `SliceLength`
- `Scratch`

These should not be treated as ordinary automation lanes in the first merged version.

Recommended handling:

- move them into a `Trigger FX` or `Performance FX` section in Scene Mode
- keep them visually near triggers, not with the continuous lanes

This avoids forcing unlike concepts into one lane model.

## Strip already playing when scene changes

Rule:

- Scene Base applies immediately
- Motion also switches immediately

For `FollowStrip`:

- it locks to the strip's current loop phase if the strip is already active
- it does not wait for next trigger

This is simpler than maintaining shadow state.

## Multiple triggers in a scene

Every trigger event resets `FollowStrip` motion for that strip instance.

That makes the behavior musically obvious.

## One-shots and sample strips

`FollowStrip` remains valid, but its lifetime may be short.

UI recommendation:

- allow it
- but dim the lane when the strip is not active

## Migration From Current Mod Slots

The current engine allows:

- 6 mod slots per strip
- arbitrary targets
- overlapping ownership

That is too much for the merged system.

Migration should simplify.

## Migration target

Move to:

- one Motion lane per target per strip

## Migration algorithm

For each strip:

1. collect current mod slots by target
2. ignore slots with `depth <= 0`
3. for each target:
   - if exactly one slot targets it:
     - convert directly into the strip's Motion lane
   - if multiple slots target it:
     - choose one canonical source
     - preserve the others as disabled imported backups
     - mark the scene as needing review

## Canonical source selection

Choose in this order:

1. active mod slot
2. highest depth slot
3. longest non-empty lane
4. first slot

## Imported backup handling

Do not silently discard legacy data.

Store extra slots in a legacy block:

```cpp
struct LegacyMotionImport
{
    int originalSlot;
    SceneMotionLane lane;
    bool enabled;
};
```

This gives safe migration without keeping the old mental model in the main UI.

## Scene migration

When opening older sessions:

- current scene clips remain as Base + Trigger data
- current mod slots migrate into strip Motion lanes
- if a scene has no clip data but the strip has active mod data, the Scene tab still shows Motion lanes

## Group Mode

Group mode remains unchanged.

The merged Scene + Motion system only applies when Scene Mode is active.

Outside Scene Mode:

- normal strip workflows stay intact
- any dedicated advanced modulation editor can remain temporarily during migration

## Main Logic Problems To Avoid

## Problem 1: double ownership

If Scene Base and Motion are both editable but the user cannot tell which one owns the sound, editing becomes confusing.

Fix:

- always show Base / Motion / Result distinctly

## Problem 2: recording the heard result by accident

This creates double modulation.

Fix:

- default record path writes Base only

## Problem 3: pretending FollowStrip is linear

That is visually wrong.

Fix:

- draw it as strip-local motion, not a fake full-width scene line

## Problem 4: too many simultaneous motion lanes

Six overlapping legacy mod slots is not intuitive.

Fix:

- one Motion lane per target

## Problem 5: transport naming

`Free` / `Sync` hides the actual behavior.

Fix:

- `SceneLoop`
- `FollowStrip`

## Recommended Implementation Order

### Phase 1

- rename transport modes in UI
- add merged lane chips in Scene tab
- show Base / Motion / Result for current targets
- show `FollowStrip` as strip-local overlay

### Phase 2

- make Scene tab the editor for Motion lanes
- hide Mod page when Scene Mode is on
- keep old engine storage temporarily

### Phase 3

- migrate storage from legacy mod slots to `SceneMotionLane`
- add bake commands
- add migration warnings / imported legacy backup handling

### Phase 4

- retire separate Mod page entirely if no longer needed

## Recommendation

If you fully merge Scene and Mod, the cleanest product model is:

- scenes stay linear
- motion lanes are children of the scene
- motion lanes declare their own timing domain
- `FollowStrip` remains strip-local
- recording writes Base
- Result is always visible but never the default record target

That gives one user-facing system without losing the expressive strip-follow behavior that makes the current modulation engine musically useful.
