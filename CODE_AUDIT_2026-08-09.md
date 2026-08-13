# mlrVST-modern — Code Audit (2026-08-09)

> **Status update (2026-08-09):**
> **Phase 1 — DONE.** §1.1 (session-state persistence + loadedPresetIndex), §1.2 (empty-scene chain
> wipe), §1.3 (insertSceneSlot chain remap), §1.5 (degraded-recall autosave guard via
> `activeSceneRecallDegraded`), §1.4/§2.1a (bulk `setSceneChainSteps` keeps playback alive; normalize
> preserves end-sample fields).
> **Phase 2 — DONE** except as noted. §2.1b (end-sample schedule survives re-arm), §2.2 (Return armed
> in the live path via `SceneScheduler::armReturnRouteForSequenceHandoff`), §2.3 (real loop flag +
> range, default loop on, play-once-hold when off, LOOP button in the chain row), §2.5 (F1–F4 favorite
> buttons in the fill summary row: click applies, Alt-click stores), §2.6 (restore-lane uses stored
> scene values via synthetic control-point events).
> **§2.4 decided:** recall/length modes stay pinned to Manual/ManualBars — the machinery is complete
> but untested; re-enabling needs hardware/DAW validation (comment added at the sanitizers).
> **§2.8 decided:** grid stays at 7 launchable scenes — the scene row is fully allocated
> (0-6 scenes, 7 recorder, 8-12 length cells, 14-15 nav); adding S8 means evicting a control, which
> is a user layout decision.

Scope: full-codebase analysis with deep focus on the arranger (scene chain/timeline) system.
Method: four parallel deep-read audits — arranger engine (SceneScheduler + scene switch/boundary
transition), arranger UI (SceneControlPanel + canvases), scene persistence/state, and a
feature-completeness sweep of the rest of the plugin. Every finding cites file:line.
CONFIRMED = code path fully traced. PLAUSIBLE = suspicious, not fully traced.

---

## Part 1 — Arranger: critical data-integrity bugs (fix first)

### 1.1 Scenes, chain, and performance clips are NOT saved in DAW session state — the code exists but is never called. CONFIRMED
`getStateInformation`/`setStateInformation` (`Source/PluginProcessorPresetState.cpp:171-234`) save only
APVTS params + paths + control pages + flip + loop-pitch. The fully implemented
`appendSceneModeStateToState` / `loadSceneModeStateFromState` (`Source/SceneScheduler.cpp:3591`, `:3768`
— including StoredScenes snapshots, chain, favorites, scenePerformanceBlob) have **zero call sites**.
`setStateInformation` also resets `loadedPresetIndex = -1` (line 207) and never restores it.
**Failure:** user builds scenes + chain in a DAW project, saves the project (not a plugin preset),
reopens → the entire arrangement, scene snapshots, timing, and performance recordings are gone.
**Fix:** call both functions from get/setStateInformation and persist/restore `loadedPresetIndex`.

### 1.2 Tapping an empty scene pad destroys the whole chain. CONFIRMED
`performEmptySceneLoad` resets `processor.sceneChainState = {}` and all per-scene timing arrays
(`Source/SceneScheduler.cpp:3448`), while `performSceneLoad` carefully preserves them (`:3486-3505`).
One stray tap on an empty slot during a set erases the arrangement.

### 1.3 Insert Before/After shifts scene content but never remaps chain-step slot references. CONFIRMED
`SceneScheduler::insertSceneSlot` (`Source/SceneScheduler.cpp:2414-2474`) moves scene content S(n)→S(n+1)
but `sceneChainState.steps[].sceneSlot` is untouched (UI wiring `Source/PluginEditorPanels.cpp:4407-4435`).
Every chain step at/after the insert point now plays the wrong scene.

### 1.4 Any structural chain edit silently stops chain playback. CONFIRMED
`applySceneChainSteps` (`Source/PluginEditorPanels.cpp:2955-2990`) starts with
`processor.clearSceneChain()`; at chainLength 0 `sanitizeSceneChainRuntimeState`
(`Source/SceneScheduler.cpp:518-548`) sets `sceneSequenceActive = false` and clears boundary-transition
state + pending recalls. Nothing re-arms after the rebuild. A single mouse-wheel tick over a step's
repeat count (`Source/PluginEditorPanels.cpp:9110-9127`) — or one inertial trackpad flick, which emits
dozens — stops the running chain and cuts any in-flight fill.
**Fix:** make chain edits in-place (or capture/restore the runtime playback state around the rebuild).

### 1.5 A missing sample file becomes permanent loss of the scene's sample reference. CONFIRMED chain
Recall of a scene whose `samplePath` is unavailable → payload build fails
(`Source/PluginProcessorPreparedScene.cpp:675-706`) → `stopStripForUnavailableScene` wipes the strip
(`Source/PluginProcessorSceneSwitch.cpp:918-1132`). Any subsequent edit queues an active-scene autosave
(`Source/PluginProcessorParameterChange.cpp:308-326`), and the autosave/boundary capture re-captures the
wiped state over the stored slot (`Source/PluginProcessorSceneAutosave.cpp:82-133`,
`Source/SceneScheduler.cpp:3119-3239`). The stored path is gone even if the drive is remounted; the next
preset save bakes the loss to disk. Note `existsAsFile` here doesn't use the `safeFileExistsAsFile`
guard used elsewhere (`Source/PluginProcessorSceneSwitch.cpp:1398-1404`) — a dead network volume can
stall the audio thread for seconds.
**Fix:** on unavailable sample, keep the stored reference and mark the slot "capture-locked" until the
strip actually has audio again.

### 1.6 Captured scenes are runtime-only; browsing to another main preset discards them silently. CONFIRMED
`persistStoredSceneSlotStatesToMainPreset` is a no-op (`Source/SceneStore.cpp:275-284`); the slot cache
is single-preset (`storedSceneSlotStateMainPresetIndex`) and cleared on preset switch. "Save scene"
succeeds visibly, then browsing presets without an explicit preset save loses all 8 scenes.
Additionally `migrateLegacyStoredSceneSlotStates` is disabled (`Source/SceneStore.cpp:155-162`) — old
presets load with no scenes (version-migration hole).

---

## Part 2 — Arranger: advertised features that are dead or broken

### 2.1 Per-step transition end-sample is doubly broken (feature is inert). CONFIRMED
(a) `normalizeSceneChainState` (`Source/SceneScheduler.cpp:437-467`) rebuilds steps copying only
sceneSlot/repeats/transition params — `transitionEndSampleFile`/`Settings` are cleared to defaults.
Every edit path normalizes, so `setSceneChainStepTransitionEndSampleFile`
(`Source/PluginProcessorSceneConfig.cpp:270-283`) is immediately wiped, returns false, and
`reloadSceneChainTransitionEndSample` never runs; serialization writes the wiped values
(`Source/SceneScheduler.cpp:1526-1538`).
(b) Even if assigned, the scheduled trigger (one bar after the boundary,
`Source/SceneScheduler.cpp:2935`) is destroyed within one audio block: `updateSceneQuantizedRecall`
either re-arms the next boundary (`:2933-2952`) or calls `clearSceneBoundaryTransitionState()` with
default `preserveScheduledEndSample=false` (`:2956`, `Source/PluginProcessor.h:1983`,
`Source/PluginProcessorSceneBoundaryTransition.cpp:396-418`).
**Fix:** preserve end-sample fields in normalize; preserve the schedule across re-arm.

### 2.2 "Return/Back" transition type is dead during live playback. CONFIRMED
The only `armSceneChainReturnOverride` call is in `processPendingSceneApply`
(`Source/SceneScheduler.cpp:3249-3261`), which early-returns while transport runs (`:3016-3020`).
The live apply path `renderPendingPreparedSceneSwitch` (`Source/PluginProcessorSceneSwitch.cpp:1622-1804`)
never arms it. A step marked "Back" just advances linearly.

### 2.3 Chain loop range and end behavior are stubs; chains always loop the full chain forever. CONFIRMED
`setSceneChainLoopEnabled`/`setSceneChainLoopRange` ignore their arguments
(`Source/SceneScheduler.cpp:1336-1371`); `normalizeSceneChainState` force-resets
loopEnabled/loopStart/loopEnd (`:469-479`), clobbering ranges loaded from XML (`:1766-1769`).
"Play once then stop/hold" is impossible; a 1-step chain can never run (`:1373-1376` requires ≥2).
The UI's loop-range preservation (`Source/PluginEditorPanels.cpp:2983-2989`) is dead code.

### 2.4 Scene recall modes and length modes are hard-disabled. CONFIRMED
`sanitizeSceneRecallMode` always returns `Manual`; `sanitizeSceneLengthMode` always `ManualBars`
(`Source/SceneScheduler.cpp:65-75`; setter clamps at `:758-778`). Dead as a result:
QuantizeGrid/PatternEnd/SceneEnd recall, LongestStrip/LongestPattern/AnchorStrip lengths, the whole
`computeNextScenePatternEndPpq` machinery (`:1860-1951`, `:2735-2769`), and `sceneAnchorStrips`
storage/persistence. Decide: implement or remove the scaffolding + hidden UI.

### 2.5 Transition favorites persist but have no entry point. CONFIRMED
`captureSceneTransitionFavorite`/`applySceneTransitionFavorite` are implemented and serialized
(`Source/PluginProcessorSceneConfig.cpp:312-350`, `Source/SceneScheduler.cpp:1541`, `:1774-1796`) but
called from no UI or monome handler.

### 2.6 "Restore lane to stored scene state" actually restores factory defaults. CONFIRMED
`restoreSceneStripControlTargetsToStoredState` forwards to `...DefaultState`
(`Source/PluginProcessorSceneMotion.cpp:554-560`) — hard-coded defaults (volume 1.0, filter off, …)
instead of the scene's stored values. `getStoredSceneControlValue`
(`Source/PluginProcessorSceneClipData.cpp:149`) exists but is unused here. Used by the editor when
clearing automation lanes (`Source/PluginEditorPanels.cpp:9359`).

### 2.7 Rearrange automation is captured/serialized but dropped on load and no-op on playback. CONFIRMED
`ScenePerformanceRecorder::applyData` drops Rearrange control points
(`Source/ScenePerformanceRecorder.cpp:1052-1056`); the playback case is empty
(`Source/PluginProcessorScenePerformance.cpp:582-583`).

### 2.8 Monome grid can launch only 7 of 8 scenes. CONFIRMED
`kMonomeSceneLaunchColumns = 7` (`Source/MonomeController.cpp:15`, `:2561`) — S8 exists in the UI but
is unreachable from the grid.

---

## Part 3 — Arranger: real-time safety (glitches exactly at scene boundaries)

### 3.1 The live scene switch does heavy non-RT-safe work on the audio thread. CONFIRMED
`renderPendingPreparedSceneSwitch` → `applyPreparedSceneSwitchPayload` runs in `processBlock`
(`Source/PluginProcessor.cpp:3604`). On the audio thread it performs:
- filesystem stats (`existsAsFile/getSize/getLastModificationTime`,
  `Source/PluginProcessorSceneSwitch.cpp:1398-1404`; `presetExists` disk check `:1765` →
  `Source/PresetStore.cpp:2337-2344`)
- Flip strips: base64 decode or full **disk read + decode** (`:1184-1192` →
  `Source/PluginProcessorPresetState.cpp:356-397`) — the preload payload doesn't pre-build Flip audio
- full sample-buffer reallocation + copy under `CriticalSection` per strip
  (`Source/AudioEngine.cpp:2156-2199` via `:696-706`)
- ~60 `juce::String` param-ID heap allocs per strip (`:993-1315`), XML clone (`:1186`),
  ValueTree alloc (`:1608`)
- `ScenePerformanceRecorder::applyData` deep-copies ~1.4 MB of clip data **while holding clipLock**
  (`Source/ScenePerformanceRecorder.cpp:816-1103`)
- `updateMonomeLEDs()` LED render + OSC sends (`Source/MonomeController.cpp:2424`)
**Failure:** dropout precisely at the musical boundary; multi-second stall on a dead volume.
**Fix:** move all of this into the prepared payload (it's built on the timer thread already —
pre-decode Flip audio, precompute param handles, defer LED/OSC + recorder copy to the message thread).

### 3.2 Multi-MB payloads can be `delete`d on the audio thread. CONFIRMED
`retirePreparedSceneSwitchPayload` falls back to `delete payload` when all 8 retirement slots are full
(`Source/PluginProcessorSceneSwitch.cpp:230-243`).

### 3.3 Chain/step/flag state is shared across threads with no synchronization. CONFIRMED
`sceneChainState.steps` (plain array) is rewritten whole by message-thread setters
(`Source/SceneScheduler.cpp:1112-1330`) while the audio thread reads it
(`:495-516`, `:2860-2952`); `sceneSequenceActive` (plain bool, `Source/PluginProcessor.h:2680`),
`sceneSequenceCurrentStepIndex`, `pendingSceneRecall`, and `activeScenePlaybackHandle`
(message path `Source/SceneScheduler.cpp:3156-3182` has no suspend) are all dual-writer.
Symptoms: transitions armed with another step's parameters mid-compaction; one extra scene switch
firing after STOP (`stopSceneChainPlayback`, `:2060-2079`, clears flags before the pending apply).
Also PLAUSIBLE: torn `SceneSwitchEvent` under dual queuers — serial check only detects change during
the read window (`Source/PluginProcessorSceneSwitch.cpp:547-646`).
**Fix direction:** double-buffered chain snapshot with atomic pointer swap; single-writer rule for
runtime flags; make STOP clear the pending apply first.

### 3.4 Autosave scene capture mutates the live engine without suspending audio. CONFIRMED
`capturePreparedSceneSwitchPayloadTemplate` flips the active mod-sequencer slot through all 6 lanes and
restores it (`Source/PluginProcessorPreparedScene.cpp:1781-1820`). The debounced autosave path
(`processPendingSceneAutosave` → `captureSceneSlotState`) runs **without** `suspendProcessing`, so a
processBlock can render with the wrong active mod lane. Same pattern in `applySceneMotionStateToEngine`
(`Source/PluginProcessorSceneMotion.cpp:471-521`).

### 3.5 Audio-thread allocations during scene-automation playback. CONFIRMED
`applySceneHeldAutomationStateAtBeat` copies the entire event vector and does O(15×33×N) scans on scene
change (`Source/PluginProcessorScenePerformance.cpp:615-665`, `:751`); param-ID String builds per
SliceLength event (`:544`).

### 3.6 Split-block switch double-processes audio. CONFIRMED
Outgoing scene renders the FULL block, then incoming overwrites `[offset,end)`
(`Source/PluginProcessorSceneSwitch.cpp:1712`, `Source/PluginProcessorSceneRender.cpp:246-281`).
"Continue" strips process twice; non-anchored strips advance extra samples; end-sample voices render in
both passes (`Source/PluginProcessorSceneRender.cpp:234`, `:241`); outgoing audio is hard-cut at the
offset with no in-buffer crossfade.

### 3.7 Minor: `startTimer` callable from the audio thread (`Source/SceneScheduler.cpp:2150-2151`,
called from `Source/PluginProcessorSceneSwitch.cpp:1768`) — TimerThread lock, priority-inversion risk.

---

## Part 4 — Arranger: chain-logic and timing edge cases

- **Host cycle/loop stalls the chain forever.** Nothing invalidates `pendingSceneRecall.targetResolved`
  on a backward PPQ jump while playing (only stop resets it, `Source/SceneScheduler.cpp:2626-2633`).
  DAW loops a 4-bar region with the boundary beyond loop end → chain never advances. CONFIRMED
- **"Run chain" while transport plays reloads/re-phases the live scene** instead of legato-attaching:
  `renderPendingPreparedSceneSwitch` ignores `event.ownerOnlySwitch` entirely and realigns to a stale
  ppq/sample pair (`Source/SceneScheduler.cpp:2016-2043`, `Source/PluginProcessorSceneSwitch.cpp:1622-1804`).
  Unsaved live tweaks are discarded; strips restart. CONFIRMED
- **Pending switch hangs forever if the payload build fails** — event never consumed or abandoned
  (`Source/SceneScheduler.cpp:2999-3020`, `Source/PluginProcessorSceneSwitch.cpp:150-159`). CONFIRMED
- **Start step ignored / duplicate scenes mis-attach:** `startSceneChainPlayback` overrides the requested
  start with the FIRST chain occurrence of the active scene (`Source/SceneScheduler.cpp:1991-1998`,
  `:1383-1393`); the UI "NEXT" badge has the same first-match bug
  (`Source/PluginEditorPanels.cpp:8198-8207`). CONFIRMED
- **Transition "subtract" underflow:** an 8-beat subtracted transition on a 4-beat scene truncates it to
  0.25 beats (`Source/SceneScheduler.cpp:1846-1854`). CONFIRMED
- **Bar quantization ignores the host's bar origin** — `ceil(ppq/beatsPerBar)*beatsPerBar` instead of
  `getPpqPositionOfLastBarStart()` (`Source/SceneScheduler.cpp:2780-2784`). PLAUSIBLE (meter changes /
  offset origins quantize to the wrong bar).
- **Serial churn defeats payload caching while transport is stopped** — requeue with a new serial every
  33 ms forces a full payload rebuild per tick (`Source/SceneScheduler.cpp:3128-3154`, `:3196-3216`).
  PLAUSIBLE
- **After normalize-compaction, `sceneSequenceCurrentStepIndex` is clamped, not remapped**
  (`Source/SceneScheduler.cpp:542`) — live edits can skip/repeat a scene. CONFIRMED
- **Session load restores `scenePlaybackOwner = Chain` with `sceneSequenceActive=false`**
  (`Source/SceneScheduler.cpp:3810-3815`) — cosmetic mismatch. CONFIRMED

---

## Part 5 — Arranger UI bugs and UX gaps

Bugs (all CONFIRMED unless noted):
- **Double-clicking a transition chip cycles Type twice AND toggles timing mode** — chain canvas lacks
  the `getNumberOfClicks() == 1` guard the timeline has (`Source/PluginEditorPanels.cpp:8817-8848`,
  `:8655-8685`; guard example `:11814`).
- **Right-/Alt-click a step = instant, unconfirmed, un-undoable delete** (`:8605-8625`). There is **no
  UndoManager anywhere in Source/** and no Cmd+Z (`:9627-9696`). The 20 px transition hit band sits
  between 30 px step cells (`:3035-3050`) — a slightly-off right-click destroys a step.
- **Invisible editable transition** between the last step and the Add cell — hit test accepts the
  never-painted loopback chip at index N-1 and swallows "Add" clicks (`:3052-3061`, `:8586`, `:8223`).
- **Triggers in strips with collapsed automation lanes select but can't be dragged**; crossing an
  expanded card mid-drag jumps the trigger to that strip (`:12507-12516`, `:12534-12535`).
- **Stale focus indices after delete/reorder** retarget the fill editor and the pinned end-sample editor
  to a different step with no indication (`:7484-7485`, `:7533-7541`).
- **First click on a scene slot button after drag-into-chain is swallowed**
  (`sceneSlotDragSuppressClick`, `:12831` vs `:4494-4501`).
- **Fill editor silently re-binds to the live playback step** while the chain runs and nothing is
  selected (`getFocusedSceneChainStep` fallback, `:5542-5556`) — edits land on the wrong transition
  around boundaries.
- **Full ~700-line `refreshFromProcessor` at 20 Hz even when the Scene tab is hidden**, ending in
  unconditional repaints of both canvases (`Source/PluginEditor.cpp:9934-9938`,
  `Source/PluginEditorPanels.cpp:7448-8175`, `:8173-8174`). No dirty-checking.
- **Step-pattern painting writes directly to the engine strip with no guard**
  (`:11391-11405`). PLAUSIBLE race.
- Drag-reorder uses insert-before semantics while the highlight fills the target cell; can't drop onto
  the last position directly (`:8868-8882`).
- Multi-selection can't be dragged — click collapses to single (`:11931-11939`, `:12598`).

UX / missing standard arranger interactions:
- **Per-step repeat count**: editable only via mouse wheel, displayed nowhere on the cell
  (paint `:8406-8513`; only readout is the "S1x4" hint text).
- **No undo/redo, no chain-step copy/paste/duplicate, no context menus, no keyboard control of the
  chain, no click-to-jump/relocate into a running chain.**
- The 7-modifier wheel matrix (Style/Lead/Blend/Type/Scope/Contour/When) is documented only in one
  ~90-word tooltip (`:7493`); chips are 32×13 px with no per-chip tooltips.
- Vestigial hidden controls kept wired but unreachable: `sceneMotionEditButton` (disabled+hidden,
  `:7142`, `:6916`), `sceneChangeModeBox`, `sceneLengthModeBoxes`, `sceneAnchorStripBoxes`,
  `sceneClearButton`, `sceneDeleteButton`, `sceneClearTriggersButton`, `sceneClearControlsButton`.

---

## Part 6 — Rest of the plugin (non-arranger)

### Top bugs
1. **Sample load blocks the audio thread.** `EnhancedAudioStrip::process` takes `bufferLock` every block
   (`Source/AudioEngine.cpp:5093`); `loadSample` holds it through full-buffer copy + transient/analysis
   rebuild (`:2156-2222`). Loading via monome browser or preset while playing hitches. CONFIRMED
2. **Loop capture stalls the callback.** `captureLoop` (message thread) holds the same lock as
   `processInput` (audio thread) while copying up to ~700k+ frames (`Source/AudioEngine.cpp:1727`,
   `:1769-1818`) — dropout at the exact moment the performer resamples. CONFIRMED
3. **The "Limiter" is a hard clipper.** 14 `juce::dsp::Limiter` instances are declared/prepared/reset and
   never process a sample (`Source/AudioEngine.h:2410-2411`, `Source/AudioEngine.cpp:12546-12561`); the
   actual path is per-sample `jlimit` (`Source/AudioEnginePostProcess.cpp:218-236`). CONFIRMED
4. **MIDI-CC macros run non-RT-safe work in processBlock** — String builds, `setValueNotifyingHost`,
   and (Step strips) IIR coefficient heap allocs (`Source/PluginProcessorMacro.cpp:926-956`, `:164-190`,
   `Source/StepSampler.h:453-475`). CONFIRMED
5. **Group mute hard-gates mid-buffer** — click, frozen delay/filter tails, and skipped per-block strip
   bookkeeping while muted (`Source/AudioEngine.cpp:13589-13605`). CONFIRMED
6. **Cross-instance state leak:** function-local `static` loop-set gesture state
   (`Source/MonomeController.cpp:1096-1097`) — two instances in one process share it. CONFIRMED
7. **StepSampler filter coefficient writes race the audio thread** (non-atomic multi-word write,
   `Source/StepSampler.h:329-345`, `:453-475` vs `:381-386`); plus a latent enum-order trap
   (`Source/StepSampler.h:19-25` vs `Source/AudioEngine.h:493-498` — HP/BP swapped). CONFIRMED
8. **Monome pan can never reach full right:** `(column-8)/8` → +0.875 max
   (`Source/MonomePageHandlers.cpp:124-125`; replay `Source/PluginProcessorMonomePattern.cpp:56`). CONFIRMED
9. **16-wide grid assumptions:** volume/filter `column/15`, record/utility cells hardcoded at x==11-15 —
   a monome 64 gets ~47% max volume, ~505 Hz filter cap, no record workflow
   (`Source/MonomeMixActions.cpp:32-305`, `Source/MonomeFileBrowserActions.cpp:8-14`,
   `Source/PluginProcessorMonomeControls.cpp:183-198`). CONFIRMED
10. **No PDC for realtime-Signalsmith pitch:** `kReportRealtimeSignalsmithLatencyToHost = false`
    (`Source/PluginProcessor.cpp:194-198`) — deliberate workaround; tracks play late. CONFIRMED
11. **Unbounded debug log with per-launch file I/O:** `appendSceneDebugLog` appends to
    `mlrvst_scene_debug.log` on every scene recall/apply (`Source/PluginProcessorSceneState.cpp:47-59`,
    call sites incl. `Source/SceneScheduler.cpp:3050`, `:3281`, `:3336`). CONFIRMED

### Partially wired features
- **Pattern-recorder control events: playback fully built, recording orphaned.**
  `recordControlEvent` has zero callers (`Source/AudioEngine.cpp:1518`); the 180-line replay switch
  (`Source/PluginProcessorMonomePattern.cpp:28-206`) is unreachable in non-scene mode. Recording
  filter/volume/pitch rides into patterns is ~80% built. CONFIRMED
- **Moog Huovilainen filter selectable but silently substituted** (compile flag defaults OFF,
  `CMakeLists.txt:13`; Stilson substituted `Source/AudioEngineFx.cpp:235-240`; tooltip discloses).
- **OneShot/Gate/Flip modes unreachable from the grid** — Group page offers only Loop/Step/Grain
  (`Source/MonomeGroupAssignActions.cpp:42-53`).
- Dead code kept: `MonomeFilterActions` press handlers (`Source/MonomeFilterActions.cpp:6-85`),
  legacy `LiveRecorder` stubs (`Source/AudioEngine.cpp:1709-1720`), `StepSampler::tempAudioFile`,
  `Source/*.bak` snapshots, stale `Docs/AUDIO_ENGINE_DOCS.md` (says 8 strips; engine is 14).

### Memory ceilings (PLAUSIBLE, bounds real but high)
Embedded WAV base64 stored twice per scene (strip state + snapshot XML) × 8 scenes, up to ~64 MB per
copy; `sceneFileAudioCache` LRU up to ~768 MB (24 × 32 MB, `Source/PluginProcessorPreparedScene.cpp:24-25`).

### Verified clean (negative results)
Chain XML save/load symmetric; recorder binary format bounds-checked + versioned (v1-v4);
prepareToPlay/releaseResources reset thoroughly; preset load hardened (size caps, clamps,
exception-safe); OSC on message thread; APVTS params all traced to consumers; time-stretch backend
gating done right; preload payload builds on the timer thread as designed.

---

## Part 7 — Recommended fix order

**Phase 1 — data integrity (small diffs, huge user impact)**
1. Wire scene/chain/performance state into DAW session save/load (§1.1) + restore `loadedPresetIndex`.
2. Stop `performEmptySceneLoad` from wiping the chain (§1.2).
3. Remap chain-step slot refs in `insertSceneSlot` (§1.3).
4. Preserve end-sample fields in `normalizeSceneChainState` (§2.1a) — also unblocks the feature.
5. Guard autosave against capturing a wiped strip after missing-file recall (§1.5).
6. Keep chain playback alive across chain edits (§1.4).

**Phase 2 — make advertised features real (or remove the UI)**
7. End-sample trigger schedule preservation (§2.1b); Return transition in the live path (§2.2);
   chain end behavior + loop range (§2.3); decide recall/length modes (§2.4); favorites entry point
   (§2.5); fix restore-lane-to-stored (§2.6); 8th scene on the grid (§2.8).

**Phase 3 — RT safety (removes boundary glitches)**
8. Move all switch-apply heavy work into the prepared payload; defer LED/recorder work to the message
   thread (§3.1); retire payloads off the audio thread (§3.2); double-buffered chain snapshot +
   single-writer flags (§3.3); autosave capture under suspend or from snapshot (§3.4);
   fix split-block double render (§3.6). Then the non-arranger RT items: sample-load double-buffer,
   loop-capture try-lock/FIFO, macro deferral, group-mute fades (§6.1-6.5).

**Phase 4 — UX polish**
9. UndoManager (JUCE has one) covering chain + clip edits; repeat-count badge on step cells;
   context menus replacing right-click-delete; double-click guard on chips; kill the invisible
   transition hit zone; stale-focus remap; dirty-flag repaints + skip refresh when hidden;
   per-chip tooltips; wire the dsp::Limiter; grid-width-relative monome math + pan fix.
