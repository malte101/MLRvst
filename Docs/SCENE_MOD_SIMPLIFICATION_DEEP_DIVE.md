# Scene + Mod Simplification Deep Dive

## Goal

Make Scene Mode feel like one coherent performance timeline instead of two overlapping modulation systems.

Today there are two separate time-based systems:

1. Scene clips
   - Record triggers and monome control gestures into scene-length event timelines.
   - Playback writes held "base" values over time.

2. Mod sequencers
   - Per-strip step sequencers with their own target, depth, transport mode, rate, offset, and shape.
   - Applied at render time on top of the strip's current parameter state.

These are both valid, but they are too similar from the user's point of view.

## What The Code Does Today

### Scene clips are base-state timelines

- Scene playback writes direct strip values in `MlrVSTAudioProcessor::playbackScenePerformanceEvent(...)`.
- That path sets speed, pitch, pan, volume, filter, delay, grain values, and trigger events.
- Reference:
  - `Source/PluginProcessor.cpp`
  - around `6635`

### Mod sequencers are render-time modifiers

- Mod sequencers do not replace the stored strip state permanently.
- During render, the engine:
  - reads the strip's original values,
  - computes each active mod lane,
  - applies the modded value for that render segment,
  - renders,
  - restores the original strip state after the segment.
- Reference:
  - `Source/AudioEngine.cpp`
  - around `11407` to `11925`

### "Sync" and "Free" are currently misleading names

- `ModTransportMode::Sync` uses `strip->getLoopPhaseNormalized()`.
- That means it follows strip playback phase and therefore follows strip offset / retrigger position.
- If not using that path, the engine falls back to host PPQ.
- So in practice:
  - `Sync` = follow strip phase
  - `Free` = host PPQ timeline
- Reference:
  - `Source/AudioEngine.h`
  - around `1690`
  - `Source/AudioEngine.cpp`
  - around `11407` to `11466`

### The Scene tab already knows there is overlap

- The Scene UI already computes:
  - held scene value,
  - current effective value,
  - whether a mod lane targets the same control.
- But it only shows overlap markers, not the actual mod pattern shape.
- Reference:
  - `Source/PluginEditorPanels.cpp`
  - around `669` to `840`

## Why It Feels Confusing

1. Scene lanes and mod lanes can target the same parameter.
2. They both look like "automation", but one is base-state automation and the other is motion.
3. The transport naming hides the most important behavior difference.
4. Scene recording captures base gestures, not the live modded result.
5. In Scene Mode, the user mentally expects one timeline editor, not one timeline plus a second hidden step system.

## Recommended Model

Do **not** duplicate mod patterns into scene clip data.

Instead, make the **Scene tab the canonical editor/view** and present each target as three layers:

1. `Base`
   - Scene automation recorded or drawn into the scene clip.

2. `Motion`
   - Mod sequencer overlay for that target.
   - Still driven by the existing mod engine.

3. `Result`
   - The effective value after Motion is applied to Base.

This keeps the DSP model stable and removes the mental split in the UI.

## Best Logic

### Core mental model

- Scene = arrangement + base parameter timeline.
- Motion = cyclic modifier on top of that base.
- Result = what you actually hear.

### Transport names

Rename the mod transport modes in the UI:

- `Host`
  - old `Free`
  - runs on host PPQ
  - ignores strip retrigger offset

- `Follow Strip`
  - old `Sync`
  - follows strip loop phase
  - retriggering or changing strip offset changes motion phase too

These names match the actual engine behavior.

### Scene recording

Default behavior in Scene Mode should be:

- Record triggers into the scene.
- Record monome control gestures into the scene `Base` layer.
- Do **not** record the live modulated `Result`.
- Do **not** silently duplicate mod pattern data into the scene clip.

Reason:

- If Scene recorded the heard result while Motion remained active, playback would double-apply modulation.
- Recording the Base layer keeps edits stable and predictable.

### When a mod lane is `Follow Strip`

This should remain offset-relative.

That is the most intuitive behavior because:

- the strip trigger defines the musical start point,
- `Follow Strip` motion should move with that playhead,
- scene trigger offset and strip-follow motion stay locked together.

So the rule should be:

- `Follow Strip` motion follows strip trigger offset.
- `Host` motion ignores strip offset and stays globally locked.

### If the user wants "what I heard" committed into the scene

Make it explicit with commands, not implicit recording:

- `Bake Motion Into Scene`
- `Bake Selected Motion Slot Into Base`
- `Suspend Motion While Recording`
- `Record Result As Base`

Only one of these should be default.

Recommended default:

- `Record Base`

Recommended optional destructive action:

- `Bake Motion Into Scene`

## UI Simplification

### In Scene Mode

Hide the standalone Mod page by default.

Replace it with a Scene-lane presentation like this:

- Trigger lane
- Base automation lane
- Motion overlay
- Effective preview

Per lane, show compact chips:

- `BASE`
- `M1 Follow Strip`
- `M2 Host`
- `FX`

Clicking a Motion chip opens a compact lane inspector:

- target
- depth
- rate
- polarity
- transport
- length
- offset
- edit step pattern

This preserves power without making the user leave the Scene tab.

### In non-Scene workflows

Keep the Mod page available as an advanced editor.

So the product model becomes:

- Scene Mode: unified timeline view
- Normal mode: dedicated Mod editor still available

## What Should Be Drawn In The Scene Tab

For every target lane:

1. Base curve
   - solid line / points

2. Motion pattern
   - translucent step ribbon or staircase
   - one overlay per active mod slot targeting that parameter

3. Effective preview
   - brighter thin line showing sampled result

4. Lane badges
   - active motion slots and transport type

If there is no Scene base data but Motion targets that lane, still show the Motion overlay.

## Recommended Product Rules

### Rule 1

There should be one visible lane per target in Scene Mode, not separate Scene and Mod lanes for the same target.

### Rule 2

Scene clips own `Base`.
Mod sequencers own `Motion`.
Do not store both copies of the same data.

### Rule 3

`Follow Strip` motion is strip-offset-relative.
`Host` motion is global.

### Rule 4

Scene recording records `Base`, not `Result`.

### Rule 5

If the user wants to commit Motion into the scene, it must be an explicit bake/freeze action.

## Options Considered

### Option A: Full merge

Delete Mod pages and store all modulation inside Scene clips.

Pros:

- one system
- very easy mental model

Cons:

- loses reusable strip motion outside Scene Mode
- large engine/data migration
- breaks existing mod-slot workflows

Verdict:

- Not the best first move.

### Option B: Soft merge in UI, keep engine split

Keep the current engine split, but make the Scene tab the main editing/view surface.

Pros:

- simplest mental model for users
- lowest migration risk
- easiest to ship incrementally
- preserves existing modulation power

Cons:

- underlying system is still dual
- needs careful overlay UI

Verdict:

- Best option.

### Option C: Keep both fully separate, only add labels

Pros:

- smallest implementation

Cons:

- confusion remains
- still feels like duplicated functionality

Verdict:

- Not enough.

## Recommended Implementation Phases

### Phase 1

- Rename UI transport labels:
  - `Free` -> `Host`
  - `Sync` -> `Follow Strip`
- Draw actual mod pattern overlays in Scene target lanes.
- Show Motion chips for active overlapping mod slots.
- Add a small lane legend:
  - Base / Motion / Result

### Phase 2

- Add a compact Motion inspector in Scene lanes.
- Allow editing active mod slot steps directly from Scene Mode.
- Hide the full Mod page while Scene Mode is active.

### Phase 3

- Add explicit bake actions:
  - bake selected motion slot into base
  - bake all motion on strip into base
  - suspend motion while recording

### Phase 4

- Re-evaluate whether the separate Mod page is still needed at all.

## Final Recommendation

The best simplification is:

- **One visual system in Scene Mode**
- **Two logical layers underneath**
  - Base = scene clip
  - Motion = mod sequencer
- **One clear rule for recording**
  - record Base, not Result
- **One clear transport vocabulary**
  - Host
  - Follow Strip

That gives the intuitive model the user wants without breaking the engine behavior that already works well for strip-relative modulation.
