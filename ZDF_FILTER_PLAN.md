# mlrVST ZDF Filter System - Complete Implementation Plan

## Phase 1: DSP ✅ (v199)
- Add juce::dsp::StateVariableTPTFilter to each strip
- Filter parameters: frequency (20-20kHz), resonance (0.1-10), type (LP/BP/HP)
- Initialize in prepareToPlay()

## Phase 2: Audio Processing (v199 continued)
- Process filter in audio chain
- Bypass when disabled

## Phase 3: GUI Tabs (v200)
- TabbedComponent for each strip
- Tab 1: Main (existing controls)
- Tab 2: FX (filter controls)

## Phase 4: Monome Button 5 (v201)
- Filter frequency control page
- Columns 0-15 = frequency (log scale)

## Phase 5: APVTS & Presets (v202)
- Add filter parameters
- Save/load in presets
