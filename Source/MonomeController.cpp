#include "PluginProcessor.h"
#include "MonomeFileBrowserActions.h"
#include "MonomeFilterActions.h"
#include "MonomeGroupAssignActions.h"
#include "MonomeMixActions.h"
#include <cmath>

void MlrVSTAudioProcessor::setMomentaryScratchHold(bool shouldEnable)
{
    if (!audioEngine)
        return;

    if (momentaryScratchHoldActive == shouldEnable)
        return;

    momentaryScratchHoldActive = shouldEnable;
    const double hostPpqNow = audioEngine->getTimelineBeat();

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;

        const auto idx = static_cast<size_t>(i);
        if (shouldEnable)
        {
            strip->captureMomentaryPhaseReference(hostPpqNow);

            momentaryScratchSavedAmount[idx] = strip->getScratchAmount();
            momentaryScratchSavedDirection[idx] = strip->getDirectionMode();
            momentaryScratchWasStepMode[idx] = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);

            // Force temporary scratch timing profile and enable scratch globally.
            strip->setScratchAmount(15.0f);

            // In Step mode, temporarily switch to Random behavior while held.
            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Random);
        }
        else
        {
            const int64_t nowSample = audioEngine->getGlobalSampleCount();

            strip->setScratchAmount(momentaryScratchSavedAmount[idx]);

            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(momentaryScratchSavedDirection[idx]);

            // Ensure no strip remains in half-finished scratch after momentary
            // modifier release; force return to timeline phase.
            if (strip->isScratchActive())
                strip->snapToTimeline(nowSample);

            // Final bulletproof phase guard: compare against pre-momentary phase
            // reference and hard-correct if drifted.
            strip->enforceMomentaryPhaseReference(hostPpqNow, nowSample);
        }
    }
}

void MlrVSTAudioProcessor::handleMonomeKeyPress(int x, int y, int state)
{
    if (!audioEngine) return;
    
    const int GROUP_ROW = 0;
    const int CONTROL_ROW = 7;
    const int FIRST_STRIP_ROW = 1;
    const auto isPresetCell = [](int gridX, int gridY)
    {
        return gridX >= 0 && gridX < PresetColumns
            && gridY >= 0 && gridY < PresetRows;
    };
    const auto toPresetIndex = [](int gridX, int gridY)
    {
        return (gridY * PresetColumns) + gridX;
    };
    const bool presetModeActive = (controlModeActive && currentControlMode == ControlMode::Preset);
    
    static int loopSetFirstButton = -1;
    static int loopSetStrip = -1;
    
    if (state == 1) // Key down
    {
        // GROUP ROW (y=0): Groups 0-3 + Pattern Recorders 4-7
        if (y == GROUP_ROW)
        {
            if (presetModeActive && isPresetCell(x, y))
            {
                const int presetIndex = toPresetIndex(x, y);
                auto nowMs = juce::Time::getMillisecondCounter();
                auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
                auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
                auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
                auto& pressStart = presetPadPressStartMs[static_cast<size_t>(presetIndex)];
                auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

                held = true;
                holdSaved = false;
                deletedTap = false;
                pressStart = nowMs;

                const uint32_t delta = nowMs - lastTap;
                if (delta <= presetDoubleTapMs)
                {
                    deletedTap = true;
                    deletePreset(presetIndex);
                    lastTap = 0;
                }

                updateMonomeLEDs();
                return;
            }

            // In Monome control-page modes, top row is reserved/disabled except Modulation.
            // This prevents accidental access to group/pattern/scratch/transient controls.
            if (controlModeActive
                && currentControlMode != ControlMode::Normal
                && currentControlMode != ControlMode::Modulation)
            {
                return;
            }

            // Restore ONLY row 0 col 8 momentary scratch hold.
            if (x == 8 && (!controlModeActive || currentControlMode == ControlMode::Normal))
            {
                setMomentaryScratchHold(true);
                updateMonomeLEDs();
                return;
            }

            // Row 0 cols 9..14 remain intentionally unused (time/trans not implemented).

            // FILTER MODE: Buttons 0-3 select filter sub-pages
            if (controlModeActive && currentControlMode == ControlMode::Filter)
            {
                if (x == 0)
                {
                    filterSubPage = FilterSubPage::Frequency;
                    updateMonomeLEDs();
                    return;
                }
                else if (x == 1)
                {
                    filterSubPage = FilterSubPage::Resonance;
                    updateMonomeLEDs();
                    return;
                }
                else if (x == 3)  // Skip button 2, use button 3 for Type
                {
                    filterSubPage = FilterSubPage::Type;
                    updateMonomeLEDs();
                    return;
                }
                // Buttons 4-7 (patterns) are disabled in Filter mode
                return;  // Don't process any other buttons on GROUP_ROW in Filter mode
            }

            if (controlModeActive && currentControlMode == ControlMode::Modulation)
            {
                const int targetStrip = juce::jlimit(0, 5, getLastMonomePressedStripRow());
                auto* engine = getAudioEngine();
                if (!engine)
                    return;
                const bool bipolar = engine->isModBipolar(targetStrip);
                const float normalizedY = 1.0f; // y=0 is highest value
                float value = normalizedY;
                if (bipolar)
                {
                    const float signedValue = (normalizedY * 2.0f) - 1.0f;
                    value = juce::jlimit(0.0f, 1.0f, (signedValue * 0.5f) + 0.5f);
                }
                engine->setModStepValue(targetStrip, x, value);

                updateMonomeLEDs();
                return;
            }
            
            // NORMAL MODE: Columns 0-3 = Group mute/unmute
            if (x < 4)
            {
                auto* group = audioEngine->getGroup(x);
                if (group)
                {
                    // Toggle mute state
                    bool wasMuted = group->isMuted();
                    group->setMuted(!wasMuted);
                    
                    // If we just muted, stop all strips in the group
                    if (!wasMuted)  // Was playing, now muted
                    {
                        auto strips = group->getStrips();
                        for (int stripIdx : strips)
                        {
                            if (auto* strip = audioEngine->getStrip(stripIdx))
                                strip->stop(false);
                        }
                    }
                    else // Was muted, now unmuted: resume group strips in PPQ sync
                    {
                        const double restartTimelineBeat = audioEngine->getTimelineBeat();
                        const double restartTempo = audioEngine->getCurrentTempo();
                        const int64_t restartGlobalSample = audioEngine->getGlobalSampleCount();
                        const auto& strips = group->getStrips();
                        for (int stripIdx : strips)
                        {
                            if (auto* strip = audioEngine->getStrip(stripIdx))
                            {
                                if (!strip->hasAudio())
                                    continue;

                                const int restartColumn = juce::jlimit(0, 15, strip->getCurrentColumn());
                                if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                                {
                                    // Step mode follows global clock directly.
                                    strip->startStepSequencer();
                                    continue;
                                }

                                strip->restorePresetPpqState(true,
                                                             strip->isPpqTimelineAnchored(),
                                                             strip->getPpqTimelineOffsetBeats(),
                                                             restartColumn,
                                                             restartTempo,
                                                             restartTimelineBeat,
                                                             restartGlobalSample);
                            }
                        }
                    }
                }
            }
            // Columns 4-7: Pattern recorders (manual stop with auto-quantized length)
            else if (x >= 4 && x <= 7)
            {
                int patternIndex = x - 4;  // 0-3 for patterns 0-3
                auto* pattern = audioEngine->getPattern(patternIndex);
                if (pattern)
                {
                    DBG("Monome pattern button " << patternIndex << " pressed");
                    
                    if (pattern->isRecording())
                    {
                        // Stop/quantize/play behavior is handled centrally in audio engine.
                        audioEngine->stopPatternRecording(patternIndex);
                    }
                    else if (pattern->isPlaying())
                    {
                        // Stop playback
                        DBG("  Stopping playback");
                        pattern->stopPlayback();
                    }
                    else
                    {
                        // Start recording with max duration; manual stop quantizes to bars.
                        audioEngine->startPatternRecording(patternIndex);
                    }
                }
            }
        }
        // CONTROL ROW - Mode buttons
        else if (y == CONTROL_ROW)
        {
            if (x >= 0 && x < NumControlRowPages)
            {
                const auto selectedMode = getControlModeForControlButton(x);
                if (isControlPageMomentary())
                {
                    currentControlMode = selectedMode;
                    controlModeActive = true;
                }
                else
                {
                    if (controlModeActive && currentControlMode == selectedMode)
                    {
                        currentControlMode = ControlMode::Normal;
                        controlModeActive = false;
                    }
                    else
                    {
                        currentControlMode = selectedMode;
                        controlModeActive = true;
                    }
                }

                updateMonomeLEDs();  // Force immediate LED update
                return;  // Don't process as strip trigger!
            }
            else if (x == 15)
            {
                quantizeEnabled = !quantizeEnabled;
                return;  // Don't process as strip trigger!
            }
        }
        // STRIP ROWS
        else if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            if (presetModeActive && isPresetCell(x, y))
            {
                const int presetIndex = toPresetIndex(x, y);
                auto nowMs = juce::Time::getMillisecondCounter();
                auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
                auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
                auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
                auto& pressStart = presetPadPressStartMs[static_cast<size_t>(presetIndex)];
                auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

                held = true;
                holdSaved = false;
                deletedTap = false;
                pressStart = nowMs;

                const uint32_t delta = nowMs - lastTap;
                if (delta <= presetDoubleTapMs)
                {
                    deletedTap = true;
                    deletePreset(presetIndex);
                    lastTap = 0;
                }

                updateMonomeLEDs();
                return;
            }

            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex < 6 && x < 16)
            {
                if (!(controlModeActive && (currentControlMode == ControlMode::GrainSize
                    || currentControlMode == ControlMode::Modulation)))
                    lastMonomePressedStripRow.store(stripIndex, std::memory_order_release);
                auto* strip = audioEngine->getStrip(stripIndex);
                if (!strip) 
                {
                    // Clear any stale loop setting state
                    loopSetFirstButton = -1;
                    loopSetStrip = -1;
                    return;
                }
                
                // Loop length setting mode - ONLY if scratch is disabled
                if (loopSetFirstButton >= 0 && loopSetStrip == stripIndex && strip->getScratchAmount() == 0.0f)
                {
                    int start = juce::jmin(loopSetFirstButton, x);
                    int end = juce::jmax(loopSetFirstButton, x) + 1;
                    
                    // Detect reverse: first button > second button
                    bool shouldReverse = (loopSetFirstButton > x);
                    
                    queueLoopChange(stripIndex, false, start, end, shouldReverse);
                    
                    DBG("Inner loop set: " << start << "-" << end << 
                        (shouldReverse ? " (REVERSE)" : " (NORMAL)"));
                    
                    loopSetFirstButton = -1;
                    loopSetStrip = -1;
                }
                // Control modes - adjust parameters
                else if (controlModeActive && currentControlMode != ControlMode::Normal)
                {
                    if (currentControlMode == ControlMode::Speed)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::Pitch)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::Pan)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::Volume)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::Swing)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::Gate)
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                    }
                    else if (currentControlMode == ControlMode::GrainSize)
                    {
                        const int targetStripIndex = juce::jlimit(0, 5, getLastMonomePressedStripRow());
                        if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                            MonomeMixActions::handleGrainPageButtonPress(*targetStrip, stripIndex, x);
                    }
                    else if (currentControlMode == ControlMode::Filter)
                    {
                        MonomeFilterActions::handleButtonPress(*strip, x, static_cast<int>(filterSubPage));
                    }
                    else if (currentControlMode == ControlMode::FileBrowser)
                    {
                        MonomeFileBrowserActions::handleButtonPress(*this, *strip, stripIndex, x);
                    }
                    else if (currentControlMode == ControlMode::GroupAssign)
                    {
                        if (MonomeGroupAssignActions::handleButtonPress(*audioEngine, stripIndex, x))
                            updateMonomeLEDs();
                    }
                    else if (currentControlMode == ControlMode::Modulation)
                    {
                        const int targetStrip = juce::jlimit(0, 5, getLastMonomePressedStripRow());
                        const bool bipolar = audioEngine->isModBipolar(targetStrip);
                        const float normalizedY = juce::jlimit(0.0f, 1.0f, (6.0f - static_cast<float>(y)) / 6.0f);
                        float value = normalizedY;
                        if (bipolar)
                        {
                            // In bipolar mode, center row maps to 0.5 and extremes map to 0/1.
                            const float signedValue = (normalizedY * 2.0f) - 1.0f;
                            value = juce::jlimit(0.0f, 1.0f, (signedValue * 0.5f) + 0.5f);
                        }
                        audioEngine->setModStepValue(targetStrip, x, value);
                        updateMonomeLEDs();
                    }
                }
                else
                {
                    // Normal playback trigger (only if strip has sample)
                    if (strip->hasAudio())
                    {
                        // Always notify strip of press for scratch hold-state.
                        // Actual scratch motion still starts when trigger fires,
                        // so quantized scheduling remains sample-accurate.
                        int64_t globalSample = audioEngine->getGlobalSampleCount();
                        strip->onButtonPress(x, globalSample);
                        
                        // Trigger the strip (quantized or immediate)
                        triggerStrip(stripIndex, x);
                        
                        // Set up for potential loop range setting
                        loopSetFirstButton = x;
                        loopSetStrip = stripIndex;
                    }
                    // If no sample loaded, do nothing (just show visual feedback via LEDs)
                }
            }
        }
    }
    else if (state == 0) // Key up
    {
        if (presetModeActive && isPresetCell(x, y))
        {
            const int presetIndex = toPresetIndex(x, y);
            auto nowMs = juce::Time::getMillisecondCounter();
            auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
            auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
            auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
            auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

            if (held && !holdSaved && !deletedTap)
                loadPreset(presetIndex);

            held = false;
            holdSaved = false;
            deletedTap = false;
            lastTap = nowMs;

            updateMonomeLEDs();
            return;
        }

        if (y == GROUP_ROW && x == 8)
        {
            setMomentaryScratchHold(false);
            updateMonomeLEDs();
            return;
        }

        // Notify strip of button release (for musical scratching)
        if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex < 6 && x < 16)
            {
                auto* strip = audioEngine->getStrip(stripIndex);
                if (strip)
                {
                    int64_t globalSample = audioEngine->getGlobalSampleCount();
                    strip->onButtonRelease(x, globalSample);
                }
            }
        }
        
        // Handle gate mode - stop strip on key release
        if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex < 6 && x < 16)
            {
                auto* strip = audioEngine->getStrip(stripIndex);
                if (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Gate)
                {
                    strip->stop(true);  // Immediate stop
                }
            }
        }
        
        // Release control mode in momentary behavior (control-page buttons)
        if (isControlPageMomentary() && y == CONTROL_ROW && (x >= 0 && x < NumControlRowPages))
        {
            currentControlMode = ControlMode::Normal;
            controlModeActive = false;
            updateMonomeLEDs();  // Update LEDs when returning to normal
        }
        
        // Reset loop setting
        if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex == loopSetStrip && x == loopSetFirstButton)
            {
                loopSetFirstButton = -1;
                loopSetStrip = -1;
            }
        }
    }
    
    updateMonomeLEDs();
}
void MlrVSTAudioProcessor::updateMonomeLEDs()
{
    if (!monomeConnection.isConnected() || !audioEngine)
        return;
    
    const int GROUP_ROW = 0;
    const int FIRST_STRIP_ROW = 1;
    const int CONTROL_ROW = 7;
    
    // Temporary LED state
    int newLedState[16][8] = {{0}};
    const double beatNow = audioEngine->getTimelineBeat();
    const bool fastBlinkOn = std::fmod(beatNow * 2.0, 1.0) < 0.5;  // Twice per beat
    const bool slowBlinkOn = std::fmod(beatNow, 1.0) < 0.5;        // Once per beat

    if (controlModeActive && currentControlMode == ControlMode::Preset)
    {
        const auto nowMs = juce::Time::getMillisecondCounter();

        for (int y = 0; y < PresetRows; ++y)
        {
            for (int x = 0; x < PresetColumns; ++x)
            {
                const int presetIndex = y * PresetColumns + x;
                const auto idx = static_cast<size_t>(presetIndex);

                if (presetPadHeld[idx] && !presetPadHoldSaveTriggered[idx])
                {
                    const uint32_t elapsed = nowMs - presetPadPressStartMs[idx];
                    if (elapsed >= presetHoldSaveMs)
                    {
                        savePreset(presetIndex);
                        presetPadHoldSaveTriggered[idx] = true;
                    }
                }

                const bool exists = presetExists(presetIndex);
                int level = exists ? 8 : 2;  // Existing lit, empty dim.
                if (presetIndex == loadedPresetIndex && exists)
                    level = slowBlinkOn ? 15 : 0;  // Loaded preset blinks.
                newLedState[x][y] = level;
            }
        }

        // Keep control row visible while preset grid is active.
        for (int x = 0; x < NumControlRowPages; ++x)
            newLedState[x][CONTROL_ROW] = 5;
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages)
            newLedState[activeButton][CONTROL_ROW] = 15;
        newLedState[15][CONTROL_ROW] = quantizeEnabled ? 12 : 3;

        for (int y = 0; y < 8; ++y)
        {
            for (int x = 0; x < 16; ++x)
            {
                if (newLedState[x][y] != ledCache[x][y])
                {
                    monomeConnection.setLEDLevel(x, y, newLedState[x][y]);
                    ledCache[x][y] = newLedState[x][y];
                }
            }
        }

        return;
    }

    // ROW 0: Group status (0-3) + Pattern recorders (4-7)
    // BUT in Filter mode: Buttons 0,1,3 = sub-page selectors
    if (controlModeActive
        && currentControlMode != ControlMode::Normal
        && currentControlMode != ControlMode::Modulation
        && currentControlMode != ControlMode::Preset)
    {
        // In non-modulation control pages, top row is intentionally disabled.
        for (int i = 0; i < 16; ++i)
            newLedState[i][GROUP_ROW] = 0;
    }
    else if (currentControlMode == ControlMode::Filter && controlModeActive)
    {
        // Filter sub-page indicators
        newLedState[0][GROUP_ROW] = (filterSubPage == FilterSubPage::Frequency) ? 15 : 5;  // Frequency
        newLedState[1][GROUP_ROW] = (filterSubPage == FilterSubPage::Resonance) ? 15 : 5;  // Resonance
        newLedState[2][GROUP_ROW] = 0;  // Unused (skip button 2)
        newLedState[3][GROUP_ROW] = (filterSubPage == FilterSubPage::Type) ? 15 : 5;       // Type
        
        // Patterns disabled in Filter mode (columns 4-7 off)
        for (int i = 4; i < 8; ++i)
            newLedState[i][GROUP_ROW] = 0;
    }
    else if (currentControlMode == ControlMode::Modulation && controlModeActive)
    {
        const int targetStrip = juce::jlimit(0, 5, getLastMonomePressedStripRow());
        const auto seq = audioEngine->getModSequencerState(targetStrip);
        const int activeStep = audioEngine->getModCurrentStep(targetStrip);
        const bool stripPlaying = audioEngine->getStrip(targetStrip) && audioEngine->getStrip(targetStrip)->isPlaying();
        const int displayRow = 0; // Top row is highest value in modulation mode.

        auto valueToRow = [&](float v)
        {
            v = juce::jlimit(0.0f, 1.0f, v);
            if (seq.bipolar)
            {
                const float signedV = (v * 2.0f) - 1.0f;
                const float n = (signedV + 1.0f) * 0.5f;
                return juce::jlimit(0, 6, static_cast<int>(std::round((1.0f - n) * 6.0f)));
            }
            return juce::jlimit(0, 6, static_cast<int>(std::round((1.0f - v) * 6.0f)));
        };

        for (int x = 0; x < 16; ++x)
        {
            newLedState[x][GROUP_ROW] = 0;
            const float v = seq.steps[static_cast<size_t>(x)];
            const int pointRow = valueToRow(v);

            if (seq.curveMode)
            {
                int level = (displayRow == pointRow) ? 10 : 1;
                if (x < 15)
                {
                    const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)]);
                    const int minRow = juce::jmin(pointRow, nextRow);
                    const int maxRow = juce::jmax(pointRow, nextRow);
                    if (displayRow >= minRow && displayRow <= maxRow)
                        level = juce::jmax(level, 6);
                }
                newLedState[x][GROUP_ROW] = level;
            }
            else
            {
                const int baseRow = seq.bipolar ? 3 : 6;
                const int minRow = juce::jmin(baseRow, pointRow);
                const int maxRow = juce::jmax(baseRow, pointRow);
                if (displayRow >= minRow && displayRow <= maxRow)
                    newLedState[x][GROUP_ROW] = (displayRow == pointRow) ? 10 : 5;
            }

            if (stripPlaying && x == activeStep)
                newLedState[x][GROUP_ROW] = juce::jmax(newLedState[x][GROUP_ROW], 15);
        }
    }
    else
    {
        // Normal mode: Groups 0-3 + Patterns 4-7
        for (int groupId = 0; groupId < 4; ++groupId)
    {
        auto* group = audioEngine->getGroup(groupId);
        if (group)
        {
            bool anyPlaying = false;
            bool isMuted = group->isMuted();
            bool hasStrips = !group->getStrips().empty();
            
            // Check if any strip in this group is playing
            if (!isMuted && hasStrips)
            {
                auto strips = group->getStrips();
                for (int stripIdx : strips)
                {
                    if (auto* strip = audioEngine->getStrip(stripIdx))
                    {
                        if (strip->isPlaying())
                        {
                            anyPlaying = true;
                            break;
                        }
                    }
                }
            }
            
            // LED brightness based on state:
            // - BRIGHT (15): Group has strips playing
            // - MEDIUM (8): Group has strips assigned but not playing
            // - DIM (3): Group is muted
            // - OFF (0): Group is empty
            if (anyPlaying)
                newLedState[groupId][GROUP_ROW] = 15;  // Playing
            else if (isMuted)
                newLedState[groupId][GROUP_ROW] = 3;   // Muted
            else if (hasStrips)
                newLedState[groupId][GROUP_ROW] = 8;   // Has strips but not playing
            else
                newLedState[groupId][GROUP_ROW] = 0;   // Empty group
        }
    }
    
        // Row 0, columns 4-7: Pattern recorder status (normal mode only)
        for (int i = 0; i < 4; ++i)
        {
            int col = i + 4;  // Columns 4-7
            auto* pattern = audioEngine->getPattern(i);
            if (pattern)
            {
                if (pattern->isRecording())
                    newLedState[col][GROUP_ROW] = fastBlinkOn ? 15 : 0;  // Recording: Fast blink
                else if (pattern->isPlaying())
                    newLedState[col][GROUP_ROW] = slowBlinkOn ? 12 : 0;  // Playing: Slow blink
                else
                    newLedState[col][GROUP_ROW] = 3;   // Stopped/idle: Dim
            }
        }
    }  // End else (normal mode)

    // Row 0 col 8: momentary scratch indicator in normal mode.
    if (!controlModeActive || currentControlMode == ControlMode::Normal)
        newLedState[8][GROUP_ROW] = momentaryScratchHoldActive ? 15 : 4;

    // Row 0 columns 9..14 intentionally left unused (time/trans not implemented).
    
    // ROWS 1-6: Strip displays
    for (int stripIndex = 0; stripIndex < 6; ++stripIndex)
    {
        int y = FIRST_STRIP_ROW + stripIndex;
        auto* strip = audioEngine->getStrip(stripIndex);
        
        if (!strip)
            continue;
        
        // Skip empty strips ONLY in Normal mode (not in control modes)
        // In control modes, we always want to show the control LEDs even on empty strips
        bool hasContent = strip->hasAudio();
        if (strip->playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            // In step mode, check if stepSampler has audio
            hasContent = strip->stepSampler.getHasAudio();
        }
        
        // Only skip empty strips when in Normal mode or FileBrowser mode
        if (!hasContent && currentControlMode == ControlMode::Normal)
            continue;
        
        // Check if group is muted
        bool isGroupMuted = false;
        int groupId = strip->getGroup();
        if (groupId >= 0 && groupId < 4)
        {
            auto* group = audioEngine->getGroup(groupId);
            if (group && group->isMuted())
                isGroupMuted = true;
        }
        
        // Different displays per mode - ONLY when control button is HELD
        if (controlModeActive && currentControlMode == ControlMode::Speed)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Pitch)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Pan)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Volume)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Swing)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Gate)
        {
            MonomeMixActions::renderRow(*strip, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::GrainSize)
        {
            const int targetStripIndex = juce::jlimit(0, 5, getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                MonomeMixActions::renderGrainPageRow(*targetStrip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Filter)
        {
            MonomeFilterActions::renderRow(*strip, y, newLedState, static_cast<int>(filterSubPage));
        }
        else if (controlModeActive && currentControlMode == ControlMode::FileBrowser)
        {
            MonomeFileBrowserActions::renderRow(*audioEngine, *strip, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::GroupAssign)
        {
            MonomeGroupAssignActions::renderRow(*strip, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Modulation)
        {
            const int selectedStrip = juce::jlimit(0, 5, getLastMonomePressedStripRow());
            const auto seq = audioEngine->getModSequencerState(selectedStrip);
            const int activeStep = audioEngine->getModCurrentStep(selectedStrip);
            const bool stripPlaying = audioEngine->getStrip(selectedStrip) && audioEngine->getStrip(selectedStrip)->isPlaying();
            const int displayRow = y; // 1..6, with row 0 rendered in GROUP_ROW branch

            auto valueToRow = [&](float v)
            {
                v = juce::jlimit(0.0f, 1.0f, v);
                if (seq.bipolar)
                {
                    const float signedV = (v * 2.0f) - 1.0f;
                    const float n = (signedV + 1.0f) * 0.5f;
                    return juce::jlimit(0, 6, static_cast<int>(std::round((1.0f - n) * 6.0f)));
                }
                return juce::jlimit(0, 6, static_cast<int>(std::round((1.0f - v) * 6.0f)));
            };

            for (int x = 0; x < 16; ++x)
            {
                newLedState[x][y] = 0;
                const float v = seq.steps[static_cast<size_t>(x)];
                const int pointRow = valueToRow(v);

                if (seq.curveMode)
                {
                    // Draw point + interpolated line to next point for readable curve graph.
                    int level = (displayRow == pointRow) ? 10 : 1;
                    if (x < 15)
                    {
                        const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)]);
                        const int minRow = juce::jmin(pointRow, nextRow);
                        const int maxRow = juce::jmax(pointRow, nextRow);
                        if (displayRow >= minRow && displayRow <= maxRow)
                            level = juce::jmax(level, 6);
                    }
                    newLedState[x][y] = level;
                }
                else
                {
                    // Step-slider mode: vertical bar to value.
                    const int baseRow = seq.bipolar ? 3 : 6;
                    const int minRow = juce::jmin(baseRow, pointRow);
                    const int maxRow = juce::jmax(baseRow, pointRow);
                    if (displayRow >= minRow && displayRow <= maxRow)
                        newLedState[x][y] = (displayRow == pointRow) ? 10 : 5;
                }

                if (stripPlaying && x == activeStep)
                    newLedState[x][y] = juce::jmax(newLedState[x][y], 15);
            }
        }
        else // Normal - playhead or step sequencer
        {
            // Check if this strip is in step mode
            if (strip->playMode == EnhancedAudioStrip::PlayMode::Step)
            {
                // STEP SEQUENCER MODE - show step pattern
                const auto visiblePattern = strip->getVisibleStepPattern();
                const int visibleCurrentStep = strip->getVisibleCurrentStep();
                for (int x = 0; x < 16; ++x)
                {
                    bool isCurrentStep = (x == visibleCurrentStep);
                    bool isActiveStep = visiblePattern[static_cast<size_t>(x)];
                    
                    if (isCurrentStep && isActiveStep)
                    {
                        // Current step AND active - brightest
                        newLedState[x][y] = 15;
                    }
                    else if (isCurrentStep)
                    {
                        // Current step but inactive - medium
                        newLedState[x][y] = 6;
                    }
                    else if (isActiveStep)
                    {
                        // Active step (not current) - medium bright
                        newLedState[x][y] = 10;
                    }
                    else
                    {
                        // Inactive step - dim
                        newLedState[x][y] = 2;
                    }
                }
            }
            else if (strip->playMode == EnhancedAudioStrip::PlayMode::Grain)
            {
                const int anchor = strip->getGrainAnchorColumn();
                const int secondary = strip->getGrainSecondaryColumn();
                const int sizeControl = strip->getGrainSizeControlColumn();
                const int heldCount = strip->getGrainHeldCount();
                const int currentCol = strip->getCurrentColumn();
                const auto preview = strip->getGrainPreviewPositions();
                const bool showScratchTrail = strip->isPlaying()
                    || (heldCount > 0)
                    || (strip->isScratchActive())
                    || (strip->getDisplaySpeed() > 0.01f);

                auto setLevelMax = [&](int x, int level)
                {
                    if (x < 0 || x >= 16)
                        return;
                    newLedState[x][y] = juce::jmax(newLedState[x][y], level);
                };

                if (heldCount <= 0 && !showScratchTrail)
                {
                    for (int x = 0; x < 16; ++x)
                        newLedState[x][y] = 0;
                    if (!isGroupMuted && strip->isPlaying() && currentCol >= 0 && currentCol < 16)
                        newLedState[currentCol][y] = 15;
                }
                else
                {
                    for (int x = 0; x < 16; ++x)
                        newLedState[x][y] = 0;

                    // Visualize grain voice "dots" as moving LED trail on the strip row.
                    // This is active while buttons are held and while scratch movement is active.
                    for (const float p : preview)
                    {
                        if (!std::isfinite(p) || p < 0.0f || p > 1.0f)
                            continue;

                        const int px = juce::jlimit(0, 15, static_cast<int>(std::round(p * 15.0f)));
                        const int dotLevel = (heldCount > 0) ? 11 : 8;
                        setLevelMax(px, dotLevel);
                    }

                    if (!isGroupMuted && strip->isPlaying() && currentCol >= 0 && currentCol < 16)
                        setLevelMax(currentCol, 7);
                    if (secondary >= 0 && secondary < 16)
                        setLevelMax(secondary, 13);
                    if (sizeControl >= 0 && sizeControl < 16)
                        setLevelMax(sizeControl, fastBlinkOn ? 15 : 3);
                    if (anchor >= 0 && anchor < 16)
                        setLevelMax(anchor, slowBlinkOn ? 15 : 10);
                }
            }
            else if (!isGroupMuted && strip->isPlaying())
            {
                // NORMAL PLAYBACK MODE - show playhead
                int currentCol = strip->getCurrentColumn();
                int loopStart = strip->getLoopStart();
                int loopEnd = strip->getLoopEnd();
                
                for (int x = loopStart; x < loopEnd && x < 16; ++x)
                    newLedState[x][y] = 2;
                
                if (currentCol >= 0 && currentCol < 16)
                    newLedState[currentCol][y] = 15;
            }
        }
    }
    
    // ROW 7: Controls
    for (int x = 0; x < NumControlRowPages; ++x)
        newLedState[x][CONTROL_ROW] = 5;

    if (controlModeActive)
    {
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages)
            newLedState[activeButton][CONTROL_ROW] = 15;
    }

    newLedState[15][CONTROL_ROW] = quantizeEnabled ? 12 : 3;
    
    // Differential update
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            if (newLedState[x][y] != ledCache[x][y])
            {
                monomeConnection.setLEDLevel(x, y, newLedState[x][y]);
                ledCache[x][y] = newLedState[x][y];
            }
        }
    }
}

//==============================================================================
