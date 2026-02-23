Stable checkpoint: preset/load safety hardening
Date: 2026-02-19

Validated after changes:
- Recording playback remains stable at loop wrap (no post-wrap audio drop)
- Preset save/load hardened against malformed values and exceptions
- Embedded sample decode now has size limits
- PluginProcessor preset wrappers now exception-safe

Files changed in this checkpoint:
- Source/PresetStore.cpp
- Source/PluginProcessor.cpp

Build verification:
- cmake --build cmake-build-debug -j4 (passed)
