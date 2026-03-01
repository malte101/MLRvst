#include "PluginProcessor.h"
#include "MonomeFileBrowserActions.h"
#include "MonomeFilterActions.h"
#include "MonomeGroupAssignActions.h"
#include "MonomeMixActions.h"
#include <array>
#include <cmath>
#include <limits>

namespace
{
double stutterDivisionBeatsFromButton(int x)
{
    static constexpr std::array<double, 7> kDivisionBeats{
        2.0,            // col 9  -> 1/2
        1.0,            // col 10 -> 1/4
        0.5,            // col 11 -> 1/8
        0.25,           // col 12 -> 1/16
        0.125,          // col 13 -> 1/32
        0.0625,         // col 14 -> 1/64
        0.03125         // col 15 -> 1/128
    };

    const int idx = juce::jlimit(0, 6, x - 9);
    return kDivisionBeats[static_cast<size_t>(idx)];
}

uint8_t stutterButtonBitForColumn(int x)
{
    if (x < 9 || x > 15)
        return 0;
    return static_cast<uint8_t>(1u << static_cast<unsigned int>(x - 9));
}

int stutterColumnFromMask(uint8_t mask)
{
    for (int bit = 6; bit >= 0; --bit)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(bit))) != 0)
            return 9 + bit;
    }
    return -1;
}
}

void MlrVSTAudioProcessor::resetStepEditVelocityGestures()
{
    stepEditVelocityGestureActive.fill(false);
    stepEditVelocityGestureStrip.fill(0);
    stepEditVelocityGestureStep.fill(0);
    stepEditVelocityGestureAnchorStart.fill(1.0f);
    stepEditVelocityGestureAnchorEnd.fill(1.0f);
    stepEditVelocityGestureAnchorValue.fill(1.0f);
    stepEditVelocityGestureLastActivityMs.fill(0);
}

void MlrVSTAudioProcessor::setMomentaryScratchHold(bool shouldEnable)
{
    if (!audioEngine)
        return;

    if (momentaryScratchHoldActive == shouldEnable)
        return;

    const double hostPpqNow = audioEngine->getTimelineBeat();
    momentaryScratchHoldActive = shouldEnable;

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

            // Original momentary scratch profile.
            strip->setScratchAmount(15.0f);

            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Random);
        }
        else
        {
            const int64_t nowSample = audioEngine->getGlobalSampleCount();
            strip->setScratchAmount(momentaryScratchSavedAmount[idx]);

            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(momentaryScratchSavedDirection[idx]);

            if (strip->isScratchActive())
                strip->snapToTimeline(nowSample);

            strip->enforceMomentaryPhaseReference(hostPpqNow, nowSample);
        }
    }
}

void MlrVSTAudioProcessor::setMomentaryStutterHold(bool shouldEnable)
{
    if (!audioEngine)
        return;

    const bool startPending = (pendingStutterStartActive.load(std::memory_order_acquire) != 0);
    const bool playbackActive = (momentaryStutterPlaybackActive.load(std::memory_order_acquire) != 0);
    if (!shouldEnable && !momentaryStutterHoldActive && !startPending && !playbackActive)
        return;

    const int64_t nowSample = audioEngine->getGlobalSampleCount();
    auto readHostTiming = [this](double& outPpq, double& outTempo)
    {
        outPpq = audioEngine ? audioEngine->getTimelineBeat() : 0.0;
        outTempo = audioEngine ? juce::jmax(1.0, audioEngine->getCurrentTempo()) : 120.0;
        if (auto* playHead = getPlayHead())
        {
            if (auto position = playHead->getPosition())
            {
                if (position->getPpqPosition().hasValue())
                    outPpq = *position->getPpqPosition();
                if (position->getBpm().hasValue() && *position->getBpm() > 1.0)
                    outTempo = *position->getBpm();
            }
        }
    };

    if (shouldEnable && (momentaryStutterHoldActive || startPending || playbackActive))
    {
        momentaryStutterHoldActive = true;
        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
        const int startQuantizeDivision = juce::jmax(1, getQuantizeDivision());
        pendingStutterStartQuantizeDivision.store(startQuantizeDivision, std::memory_order_release);
        const double entryDivision = juce::jlimit(0.03125, 4.0, momentaryStutterDivisionBeats);
        pendingStutterStartDivisionBeats.store(entryDivision, std::memory_order_release);
        audioEngine->setMomentaryStutterDivision(entryDivision);

        if (startPending && !playbackActive)
        {
            pendingStutterStartPpq.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_release);
            pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        }

        if (playbackActive)
            audioEngine->setMomentaryStutterActive(true);
        return;
    }

    if (shouldEnable)
    {
        momentaryStutterHoldActive = true;
        if (momentaryStutterButtonMask.load(std::memory_order_acquire) == 0)
        {
            const uint8_t fallbackBit = stutterButtonBitForColumn(momentaryStutterActiveDivisionButton);
            if (fallbackBit != 0)
                momentaryStutterButtonMask.store(fallbackBit, std::memory_order_release);
        }

        momentaryStutterMacroCapturePending = true;
        momentaryStutterMacroBaselineCaptured = false;
        for (auto& saved : momentaryStutterSavedState)
            saved = MomentaryStutterSavedStripState{};

        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);

        double currentPpq = 0.0;
        double tempoNow = 120.0;
        readHostTiming(currentPpq, tempoNow);
        juce::ignoreUnused(tempoNow);
        if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
        {
            // Strict PPQ safety: no valid timeline means no stutter scheduling.
            momentaryStutterHoldActive = false;
            pendingStutterStartActive.store(0, std::memory_order_release);
            pendingStutterStartPpq.store(-1.0, std::memory_order_release);
            pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
            momentaryStutterPlaybackActive.store(0, std::memory_order_release);
            audioEngine->setMomentaryStutterActive(false);
            return;
        }

        const double entryDivision = juce::jlimit(0.03125, 4.0, momentaryStutterDivisionBeats);
        const int startQuantizeDivision = juce::jmax(1, getQuantizeDivision());
        pendingStutterStartQuantizeDivision.store(startQuantizeDivision, std::memory_order_release);
        pendingStutterStartDivisionBeats.store(entryDivision, std::memory_order_release);
        pendingStutterStartPpq.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        pendingStutterStartActive.store(1, std::memory_order_release);
        momentaryStutterPlaybackActive.store(0, std::memory_order_release);
        audioEngine->setMomentaryStutterActive(false);
        return;
    }

    // UI/key state ends immediately on key-up; audio release remains quantized.
    momentaryStutterHoldActive = false;
    momentaryStutterActiveDivisionButton = -1;
    momentaryStutterButtonMask.store(0, std::memory_order_release);

    if (startPending && !playbackActive)
    {
        pendingStutterStartActive.store(0, std::memory_order_release);
        pendingStutterStartPpq.store(-1.0, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        momentaryStutterPlaybackActive.store(0, std::memory_order_release);
        momentaryStutterLastComboMask = 0;
        momentaryStutterTwoButtonStepBaseValid = false;
        momentaryStutterTwoButtonStepBase = 0;
        momentaryStutterMacroBaselineCaptured = false;
        momentaryStutterMacroCapturePending = false;
        audioEngine->setMomentaryStutterActive(false);
        audioEngine->setMomentaryStutterStartPpq(-1.0);
        audioEngine->clearMomentaryStutterStrips();
        for (auto& armed : momentaryStutterStripArmed)
            armed = false;
        return;
    }

    restoreMomentaryStutterMacroBaseline();

    if (!playbackActive)
        return;

    // Quantized stutter release (PPQ-locked):
    // convert next PPQ grid boundary to an absolute sample target now.
    const int division = juce::jmax(1, getQuantizeDivision());
    const double quantBeats = 4.0 / static_cast<double>(division);

    double currentPpq = audioEngine->getTimelineBeat();
    double tempoNow = juce::jmax(1.0, audioEngine->getCurrentTempo());
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (position->getPpqPosition().hasValue())
                currentPpq = *position->getPpqPosition();
            if (position->getBpm().hasValue() && *position->getBpm() > 1.0)
                tempoNow = *position->getBpm();
        }
    }

    if (!std::isfinite(currentPpq) || !std::isfinite(tempoNow) || tempoNow <= 0.0 || currentSampleRate <= 0.0)
    {
        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
        performMomentaryStutterReleaseNow(audioEngine->getTimelineBeat(), nowSample);
        return;
    }

    double releasePpq = std::ceil(currentPpq / quantBeats) * quantBeats;
    if (releasePpq <= (currentPpq + 1.0e-6))
        releasePpq += quantBeats;
    releasePpq = std::round(releasePpq / quantBeats) * quantBeats;

    const double samplesPerQuarter = (60.0 / tempoNow) * currentSampleRate;
    const int64_t currentAbsSample = static_cast<int64_t>(std::llround(currentPpq * samplesPerQuarter));
    const int64_t targetAbsSample = static_cast<int64_t>(std::llround(releasePpq * samplesPerQuarter));
    const int64_t deltaSamples = juce::jmax<int64_t>(1, targetAbsSample - currentAbsSample);
    const int64_t targetSample = nowSample + deltaSamples;

    pendingStutterReleaseQuantizeDivision.store(division, std::memory_order_release);
    pendingStutterReleasePpq.store(releasePpq, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(targetSample, std::memory_order_release);
    pendingStutterReleaseActive.store(1, std::memory_order_release);
}

void MlrVSTAudioProcessor::performMomentaryStutterStartNow(double hostPpqNow, int64_t nowSample)
{
    juce::ignoreUnused(nowSample);

    if (!audioEngine || !momentaryStutterHoldActive)
        return;

    double entryPpq = hostPpqNow;
    if (!std::isfinite(entryPpq))
        entryPpq = audioEngine->getTimelineBeat();
    if (!std::isfinite(entryPpq))
        return;

    const double entryDivision = juce::jlimit(
        0.03125, 4.0, pendingStutterStartDivisionBeats.load(std::memory_order_acquire));
    momentaryStutterMacroStartPpq = entryPpq;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;

    audioEngine->setMomentaryStutterDivision(entryDivision);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->setMomentaryStutterStartPpq(entryPpq);
    audioEngine->clearMomentaryStutterStrips();
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        const auto idx = static_cast<size_t>(i);
        momentaryStutterStripArmed[idx] = false;
        const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        const bool hasStepAudio = stepMode && strip->getStepSampler() && strip->getStepSampler()->getHasAudio();
        const bool hasPlayableContent = (strip && (strip->hasAudio() || hasStepAudio));
        if (!strip || !hasPlayableContent || !strip->isPlaying())
        {
            audioEngine->setMomentaryStutterStrip(i, 0, 0.0, false);
            continue;
        }

        strip->captureMomentaryPhaseReference(entryPpq);
        const int stutterColumn = juce::jlimit(0, 15, strip->getStutterEntryColumn());
        const double stutterOffsetRatio = strip->getStutterEntryOffsetRatio();
        audioEngine->setMomentaryStutterStrip(i, stutterColumn, stutterOffsetRatio, true);
        audioEngine->clearPendingQuantizedTriggersForStrip(i);
        momentaryStutterStripArmed[idx] = true;
    }
    audioEngine->setMomentaryStutterActive(true);
    momentaryStutterPlaybackActive.store(1, std::memory_order_release);
    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
}

void MlrVSTAudioProcessor::performMomentaryStutterReleaseNow(double hostPpqNow, int64_t nowSample)
{
    if (!audioEngine)
        return;

    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
    momentaryStutterPlaybackActive.store(0, std::memory_order_release);
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
    restoreMomentaryStutterMacroBaseline();
    audioEngine->setMomentaryStutterActive(false);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->setMomentaryStutterStartPpq(-1.0);
    audioEngine->clearMomentaryStutterStrips();
    momentaryStutterButtonMask.store(0, std::memory_order_release);
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;
        strip->enforceMomentaryPhaseReference(hostPpqNow, nowSample);
        momentaryStutterStripArmed[static_cast<size_t>(i)] = false;
    }
}

void MlrVSTAudioProcessor::handleMonomeKeyPress(int x, int y, int state)
{
    if (!audioEngine) return;

    const int gridWidth = getMonomeGridWidth();
    const int gridHeight = getMonomeGridHeight();
    if (x < 0 || y < 0 || x >= gridWidth || y >= gridHeight)
        return;

    const int GROUP_ROW = 0;
    const int CONTROL_ROW = getMonomeControlRow();
    const int FIRST_STRIP_ROW = 1;
    const int visibleStripCount = juce::jmax(0, getMonomeActiveStripCount());
    const int stepEditBankSize = 6;
    const int maxStepEditBank = juce::jmax(0, (visibleStripCount - 1) / stepEditBankSize);
    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
    const int maxVisibleStripIndex = juce::jmax(0, visibleStripCount - 1);
    const int stripRowsDenom = juce::jmax(1, visibleStripCount - 1);
    const int modulationRowsDenom = juce::jmax(1, visibleStripCount);
    const auto clampVisibleStrip = [maxVisibleStripIndex](int index)
    {
        return juce::jlimit(0, maxVisibleStripIndex, index);
    };
    const auto stripRowToUnit = [CONTROL_ROW, stripRowsDenom](int row)
    {
        return juce::jlimit(0.0f, 1.0f, static_cast<float>((CONTROL_ROW - 1) - row)
            / static_cast<float>(stripRowsDenom));
    };
    const auto modulationRowToUnit = [CONTROL_ROW, modulationRowsDenom](int row)
    {
        return juce::jlimit(0.0f, 1.0f, static_cast<float>((CONTROL_ROW - 1) - row)
            / static_cast<float>(modulationRowsDenom));
    };
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
    const bool stepEditModeActive = (controlModeActive && currentControlMode == ControlMode::StepEdit);
    
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

            if (stepEditModeActive)
            {
                const int bankStart = stepEditStripBank * stepEditBankSize;

                if (x >= 0 && x <= 7)
                {
                    const auto previousTool = stepEditTool;
                    switch (x)
                    {
                        case 0: stepEditTool = StepEditTool::Velocity; break;
                        case 1: stepEditTool = StepEditTool::Divide; break;
                        case 2: stepEditTool = StepEditTool::RampUp; break;
                        case 3: stepEditTool = StepEditTool::RampDown; break;
                        case 4: stepEditTool = StepEditTool::Probability; break;
                        case 5: stepEditTool = StepEditTool::Attack; break;
                        case 6: stepEditTool = StepEditTool::Decay; break;
                        case 7: stepEditTool = StepEditTool::Release; break; // Pitch tool (reusing Release slot)
                        default: break;
                    }
                    if (stepEditTool != previousTool)
                        resetStepEditVelocityGestures();

                    updateMonomeLEDs();
                    return;
                }

                if (x >= 8 && x <= 13)
                {
                    const int targetStrip = bankStart + (x - 8);
                    if (targetStrip >= 0 && targetStrip < visibleStripCount)
                    {
                        stepEditSelectedStrip = clampVisibleStrip(targetStrip);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                if (x == 14 && stepEditStripBank > 0)
                {
                    --stepEditStripBank;
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
                    const int bankStartAfter = stepEditStripBank * stepEditBankSize;
                    if (stepEditSelectedStrip < bankStartAfter
                        || stepEditSelectedStrip >= (bankStartAfter + stepEditBankSize))
                    {
                        stepEditSelectedStrip = clampVisibleStrip(bankStartAfter);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                if (x == 15 && stepEditStripBank < maxStepEditBank)
                {
                    ++stepEditStripBank;
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
                    const int bankStartAfter = stepEditStripBank * stepEditBankSize;
                    if (stepEditSelectedStrip < bankStartAfter
                        || stepEditSelectedStrip >= (bankStartAfter + stepEditBankSize))
                    {
                        stepEditSelectedStrip = clampVisibleStrip(bankStartAfter);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                return;
            }

            // In Monome control-page modes, top row is reserved/disabled except Modulation.
            // This prevents accidental access to group/pattern/scratch/transient controls.
            if (controlModeActive
                && currentControlMode != ControlMode::Normal
                && currentControlMode != ControlMode::Modulation
                && currentControlMode != ControlMode::Filter)
            {
                return;
            }

            // Row 0 col 8: original momentary scratch hold.
            if (x == 8 && (!controlModeActive || currentControlMode == ControlMode::Normal))
            {
                setMomentaryScratchHold(true);
                updateMonomeLEDs();
                return;
            }

            // Row 0, cols 9-15: momentary stutter rates (timeline-synced):
            // 9=1/4 ... 15=1/64.
            if (x >= 9 && x <= 15 && (!controlModeActive || currentControlMode == ControlMode::Normal))
            {
                const uint8_t bit = stutterButtonBitForColumn(x);
                if (bit != 0)
                    momentaryStutterButtonMask.fetch_or(bit, std::memory_order_acq_rel);
                momentaryStutterDivisionBeats = stutterDivisionBeatsFromButton(x);
                momentaryStutterActiveDivisionButton = x;
                updateMonomeLEDs();
                setMomentaryStutterHold(true);
                return;
            }

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
                const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                auto* engine = getAudioEngine();
                if (!engine)
                    return;

                // Dedicated modulation page navigation in monome mod mode.
                if (x == 11 || x == 12)
                {
                    const int delta = (x == 11) ? -1 : 1;
                    const int currentPage = engine->getModEditPage(targetStrip);
                    const int maxPage = juce::jmax(0, engine->getModLengthBars(targetStrip) - 1);
                    engine->setModEditPage(targetStrip, juce::jlimit(0, maxPage, currentPage + delta));
                    updateMonomeLEDs();
                    return;
                }

                // Slot selectors for 3 independent mod sequencers.
                if (x >= 13 && x <= 15)
                {
                    engine->setModSequencerSlot(targetStrip, x - 13);
                    updateMonomeLEDs();
                    return;
                }

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
                const bool wasStepEditMode = (controlModeActive && currentControlMode == ControlMode::StepEdit);
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

                if (controlModeActive && currentControlMode == ControlMode::StepEdit)
                {
                    if (stepEditTool == StepEditTool::Gate)
                        stepEditTool = StepEditTool::Velocity;
                    stepEditSelectedStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditSelectedStrip / stepEditBankSize);
                    resetStepEditVelocityGestures();
                }
                else if (wasStepEditMode)
                {
                    resetStepEditVelocityGestures();
                }

                updateMonomeLEDs();  // Force immediate LED update
                return;  // Don't process as strip trigger!
            }
            else if (stepEditModeActive && (x == 13 || x == 14))
            {
                const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
                if (auto* strip = audioEngine->getStrip(selectedStripIndex))
                {
                    const float currentSemitones = getPitchSemitonesForDisplay(*strip);
                    const float delta = (x == 13) ? -1.0f : 1.0f;
                    const float nextSemitones = juce::jlimit(-24.0f, 24.0f, currentSemitones + delta);
                    applyPitchControlToStrip(*strip, nextSemitones);

                    if (auto* param = parameters.getParameter("stripPitch" + juce::String(selectedStripIndex)))
                    {
                        const float normalized = juce::jlimit(0.0f, 1.0f, param->convertTo0to1(nextSemitones));
                        param->setValueNotifyingHost(normalized);
                    }
                }

                updateMonomeLEDs();
                return;
            }
            else if (x == 15)
            {
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

            if (stepEditModeActive)
            {
                if (stepEditTool == StepEditTool::Gate)
                    stepEditTool = StepEditTool::Velocity;

                const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
                auto* targetStrip = audioEngine->getStrip(selectedStripIndex);
                if (!targetStrip)
                {
                    updateMonomeLEDs();
                    return;
                }

                const float rowValue = stripRowToUnit(y);
                const float columnNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(x) / 15.0f);
                auto setStepEnabled = [targetStrip](int absoluteStep, bool shouldEnable)
                {
                    const int clampedStep = juce::jlimit(0, targetStrip->getStepTotalSteps() - 1, absoluteStep);
                    if (targetStrip->stepPattern[static_cast<size_t>(clampedStep)] != shouldEnable)
                        targetStrip->toggleStepAtIndex(clampedStep);
                };

                if (stepEditTool == StepEditTool::Attack)
                {
                    targetStrip->setStepEnvelopeAttackMs(columnNorm * 400.0f);
                    updateMonomeLEDs();
                    return;
                }

                if (stepEditTool == StepEditTool::Decay)
                {
                    targetStrip->setStepEnvelopeDecayMs(1.0f + (columnNorm * 3999.0f));
                    updateMonomeLEDs();
                    return;
                }

                if (stepEditTool == StepEditTool::Release)
                {
                    const float pitchSemitones = juce::jmap(columnNorm, -24.0f, 24.0f);
                    applyPitchControlToStrip(*targetStrip, pitchSemitones);

                    if (auto* param = parameters.getParameter("stripPitch" + juce::String(selectedStripIndex)))
                    {
                        const float normalized = juce::jlimit(0.0f, 1.0f, param->convertTo0to1(pitchSemitones));
                        param->setValueNotifyingHost(normalized);
                    }

                    updateMonomeLEDs();
                    return;
                }

                const int totalSteps = targetStrip->getStepTotalSteps();
                const int absoluteStep = targetStrip->getVisibleStepOffset() + juce::jlimit(0, 15, x);
                if (absoluteStep < 0 || absoluteStep >= totalSteps)
                {
                    updateMonomeLEDs();
                    return;
                }

                const auto stepIdx = static_cast<size_t>(absoluteStep);
                const bool wasEnabled = targetStrip->stepPattern[stepIdx];

                switch (stepEditTool)
                {
                    case StepEditTool::Gate:
                    {
                        targetStrip->toggleStepAtIndex(absoluteStep);
                        break;
                    }

                    case StepEditTool::Velocity:
                    {
                        const int column = juce::jlimit(0, MaxColumns - 1, x);
                        const uint32_t nowMs = juce::Time::getMillisecondCounter();
                        const bool sameTarget = stepEditVelocityGestureActive[static_cast<size_t>(column)]
                            && stepEditVelocityGestureStrip[static_cast<size_t>(column)] == selectedStripIndex
                            && stepEditVelocityGestureStep[static_cast<size_t>(column)] == absoluteStep;
                        const uint32_t elapsedMs = nowMs
                            - stepEditVelocityGestureLastActivityMs[static_cast<size_t>(column)];
                        const bool continueGesture = sameTarget
                            && (elapsedMs <= stepEditVelocityGestureLatchMs);

                        if (!continueGesture)
                        {
                            const float anchorStart = juce::jlimit(
                                0.0f, 1.0f, targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep));
                            const float anchorEnd = juce::jlimit(
                                0.0f, 1.0f, targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep));
                            stepEditVelocityGestureActive[static_cast<size_t>(column)] = true;
                            stepEditVelocityGestureStrip[static_cast<size_t>(column)] = selectedStripIndex;
                            stepEditVelocityGestureStep[static_cast<size_t>(column)] = absoluteStep;
                            stepEditVelocityGestureAnchorStart[static_cast<size_t>(column)] = anchorStart;
                            stepEditVelocityGestureAnchorEnd[static_cast<size_t>(column)] = anchorEnd;
                            stepEditVelocityGestureAnchorValue[static_cast<size_t>(column)] = juce::jmax(anchorStart, anchorEnd);
                        }

                        stepEditVelocityGestureLastActivityMs[static_cast<size_t>(column)] = nowMs;

                        // Bottom row (y=6) in volume tool is an explicit step-off command.
                        const bool explicitOff = (y >= (CONTROL_ROW - 1));
                        if (explicitOff)
                        {
                            setStepEnabled(absoluteStep, false);
                            targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, 0.0f, 0.0f);
                            stepEditVelocityGestureActive[static_cast<size_t>(column)] = false;
                            break;
                        }

                        const float dragShift = rowValue
                            - stepEditVelocityGestureAnchorValue[static_cast<size_t>(column)];
                        const float start = juce::jlimit(
                            0.0f,
                            1.0f,
                            stepEditVelocityGestureAnchorStart[static_cast<size_t>(column)] + dragShift);
                        const float end = juce::jlimit(
                            0.0f,
                            1.0f,
                            stepEditVelocityGestureAnchorEnd[static_cast<size_t>(column)] + dragShift);

                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        const bool shouldEnable = juce::jmax(start, end) > 0.001f;
                        setStepEnabled(absoluteStep, shouldEnable);
                        break;
                    }

                    case StepEditTool::Divide:
                    {
                        setStepEnabled(absoluteStep, true);
                        const int maxSubs = juce::jmax(2, EnhancedAudioStrip::MaxStepSubdivisions);
                        // Map row value directly to the visible subdivision scale.
                        // (Old mapping had a +2 bias that made low values feel offset.)
                        const int subdivisions = juce::jlimit(
                            2,
                            maxSubs,
                            1 + static_cast<int>(std::round(rowValue
                                * static_cast<float>(juce::jmax(1, maxSubs - 1)))));
                        targetStrip->setStepSubdivisionAtIndex(absoluteStep, subdivisions);
                        break;
                    }

                    case StepEditTool::RampUp:
                    {
                        setStepEnabled(absoluteStep, true);
                        if (rowValue <= 0.001f)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);
                        else if (targetStrip->getStepSubdivisionAtIndex(absoluteStep) <= 1)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);

                        const float baseStart = targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                        const float baseEnd = targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                        float baseMax = juce::jmax(baseStart, baseEnd);
                        if (baseMax < 0.001f)
                            baseMax = wasEnabled ? 1.0f : juce::jmax(0.25f, rowValue);

                        const float depth = rowValue;
                        const float start = juce::jlimit(0.0f, 1.0f, (1.0f - depth) * baseMax);
                        const float end = juce::jlimit(0.0f, 1.0f, baseMax);
                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        break;
                    }

                    case StepEditTool::RampDown:
                    {
                        setStepEnabled(absoluteStep, true);
                        if (rowValue <= 0.001f)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);
                        else if (targetStrip->getStepSubdivisionAtIndex(absoluteStep) <= 1)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);

                        const float baseStart = targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                        const float baseEnd = targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                        float baseMax = juce::jmax(baseStart, baseEnd);
                        if (baseMax < 0.001f)
                            baseMax = wasEnabled ? 1.0f : juce::jmax(0.25f, rowValue);

                        const float depth = rowValue;
                        const float start = juce::jlimit(0.0f, 1.0f, baseMax);
                        const float end = juce::jlimit(0.0f, 1.0f, (1.0f - depth) * baseMax);
                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        break;
                    }

                    case StepEditTool::Probability:
                    {
                        if (rowValue > 0.001f)
                            setStepEnabled(absoluteStep, true);
                        targetStrip->setStepProbabilityAtIndex(absoluteStep, rowValue);
                        break;
                    }

                    case StepEditTool::Attack:
                    case StepEditTool::Decay:
                    case StepEditTool::Release:
                    default:
                        break;
                }

                updateMonomeLEDs();
                return;
            }

            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
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
                
                // Loop length setting mode - ONLY if scratch is disabled and strip is not in Step mode.
                if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step
                    && loopSetFirstButton >= 0
                    && loopSetStrip == stripIndex
                    && strip->isButtonHeld(loopSetFirstButton)
                    && strip->getScratchAmount() == 0.0f)
                {
                    const int firstButton = juce::jlimit(0, MaxColumns - 1, loopSetFirstButton);
                    const int secondButton = juce::jlimit(0, MaxColumns - 1, x);
                    int start = juce::jmin(firstButton, secondButton);
                    int end = juce::jmax(firstButton, secondButton) + 1;

                    // Detect reverse: first button > second button
                    const bool shouldReverse = (firstButton > secondButton);

                    // Global inner-loop size divisor:
                    // 1, 1/2, 1/4, 1/8, 1/16 where 1 keeps legacy behavior.
                    const float loopLengthFactor = juce::jlimit(0.0625f, 1.0f, getInnerLoopLengthFactor());
                    if (loopLengthFactor < 0.999f)
                    {
                        const int originalLength = juce::jmax(1, end - start);
                        const int scaledLength = juce::jmax(1, static_cast<int>(
                            std::floor(static_cast<double>(originalLength) * static_cast<double>(loopLengthFactor))));

                        if (shouldReverse)
                        {
                            end = juce::jlimit(1, MaxColumns, firstButton + 1);
                            start = juce::jmax(0, end - scaledLength);
                        }
                        else
                        {
                            start = firstButton;
                            end = juce::jmin(MaxColumns, start + scaledLength);
                        }

                        start = juce::jlimit(0, MaxColumns - 1, start);
                        end = juce::jlimit(start + 1, MaxColumns, end);
                    }
                    
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
                        const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
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
                        const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                        const bool bipolar = audioEngine->isModBipolar(targetStrip);
                        const float normalizedY = modulationRowToUnit(y);
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
                    // Normal playback trigger:
                    // - Loop/Grain/Gate: requires loaded strip audio
                    // - Step mode: allow direct step toggling on main page
                    const bool canTriggerFromMainPage = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                        || strip->hasAudio();
                    if (canTriggerFromMainPage)
                    {
                        // Always notify strip of press for scratch hold-state.
                        // Actual scratch motion still starts when trigger fires,
                        // so quantized scheduling remains sample-accurate.
                        int64_t globalSample = audioEngine->getGlobalSampleCount();
                        strip->onButtonPress(x, globalSample);
                        
                        // Trigger the strip (quantized or immediate)
                        triggerStrip(stripIndex, x);
                        
                        // Set up for potential loop range setting (non-step modes only).
                        if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step)
                        {
                            loopSetFirstButton = x;
                            loopSetStrip = stripIndex;
                        }
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

        if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x >= 3 && x < (3 + BrowserFavoriteSlots))
            {
                const int slot = x - 3;
                const bool browserModeActive = controlModeActive && currentControlMode == ControlMode::FileBrowser;
                const bool favoriteWasHeld = isBrowserFavoritePadHeld(stripIndex, slot);
                if (browserModeActive || favoriteWasHeld)
                {
                    if (auto* strip = audioEngine->getStrip(stripIndex))
                    {
                        MonomeFileBrowserActions::handleButtonRelease(*this, *strip, stripIndex, x);
                        updateMonomeLEDs();
                        return;
                    }
                }
            }
        }

        if (stepEditModeActive && y == GROUP_ROW)
        {
            updateMonomeLEDs();
            return;
        }

        if (y == GROUP_ROW && x == 8)
        {
            setMomentaryScratchHold(false);
            updateMonomeLEDs();
            return;
        }
        if (y == GROUP_ROW && x >= 9 && x <= 15)
        {
            const uint8_t bit = stutterButtonBitForColumn(x);
            uint8_t currentMask = momentaryStutterButtonMask.load(std::memory_order_acquire);
            currentMask = static_cast<uint8_t>(currentMask & static_cast<uint8_t>(~bit));
            momentaryStutterButtonMask.store(currentMask, std::memory_order_release);

            if (currentMask == 0)
            {
                setMomentaryStutterHold(false);
            }
            else
            {
                const int activeColumn = stutterColumnFromMask(currentMask);
                if (activeColumn >= 9 && activeColumn <= 15)
                {
                    momentaryStutterActiveDivisionButton = activeColumn;
                    momentaryStutterDivisionBeats = stutterDivisionBeatsFromButton(activeColumn);
                    audioEngine->setMomentaryStutterDivision(momentaryStutterDivisionBeats);
                }
            }
            updateMonomeLEDs();
            return;
        }

        if (stepEditModeActive && y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            updateMonomeLEDs();
            return;
        }

        // Notify strip of button release (for musical scratching)
        if (y >= FIRST_STRIP_ROW && y < CONTROL_ROW)
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
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
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
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
            const bool wasStepEditMode = (controlModeActive && currentControlMode == ControlMode::StepEdit);
            currentControlMode = ControlMode::Normal;
            controlModeActive = false;
            if (wasStepEditMode)
                resetStepEditVelocityGestures();
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
    if (!monomeConnection.isConnected() || !audioEngine || !monomeConnection.supportsGrid())
        return;
    
    const int gridWidth = getMonomeGridWidth();
    const int gridHeight = getMonomeGridHeight();
    const int GROUP_ROW = 0;
    const int FIRST_STRIP_ROW = 1;
    const int CONTROL_ROW = getMonomeControlRow();
    const int visibleStripCount = juce::jmax(0, getMonomeActiveStripCount());
    const int stepEditBankSize = 6;
    const int maxStepEditBank = juce::jmax(0, (visibleStripCount - 1) / stepEditBankSize);
    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
    const int maxVisibleStripIndex = juce::jmax(0, visibleStripCount - 1);
    const int modulationMaxRow = juce::jmax(1, visibleStripCount);
    const auto clampVisibleStrip = [maxVisibleStripIndex](int index)
    {
        return juce::jlimit(0, maxVisibleStripIndex, index);
    };
    const auto stepEditRowNorm = [CONTROL_ROW, visibleStripCount](int row)
    {
        const int denom = juce::jmax(1, visibleStripCount - 1);
        return juce::jlimit(0.0f, 1.0f,
            static_cast<float>((CONTROL_ROW - 1) - row) / static_cast<float>(denom));
    };
    
    // Temporary LED state
    int newLedState[MaxGridWidth][MaxGridHeight] = {{0}};
    const double beatNow = audioEngine->getTimelineBeat();
    const bool fastBlinkOn = std::fmod(beatNow * 2.0, 1.0) < 0.5;  // Twice per beat
    const bool slowBlinkOn = std::fmod(beatNow, 1.0) < 0.5;        // Once per beat
    const double beatPhase = std::fmod(beatNow, 1.0);
    const bool metroPulseOn = beatPhase < 0.22;                    // Short pulse at each beat
    const int beatIndexInBar = juce::jmax(0, static_cast<int>(std::floor(beatNow)) % 4);
    const bool metroDownbeat = (beatIndexInBar == 0);
    const auto nowMs = juce::Time::getMillisecondCounter();

    if (controlModeActive && currentControlMode == ControlMode::FileBrowser)
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const auto stripIdx = static_cast<size_t>(stripIndex);
            for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
            {
                const auto slotIdx = static_cast<size_t>(slot);
                if (!browserFavoritePadHeld[stripIdx][slotIdx] || browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx])
                    continue;

                const uint32_t elapsed = nowMs - browserFavoritePadPressStartMs[stripIdx][slotIdx];
                if (elapsed < browserFavoriteHoldSaveMs)
                    continue;

                const bool saved = saveBrowserFavoriteDirectoryFromStrip(stripIndex, slot);
                browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = true;
                if (saved)
                {
                    browserFavoriteSaveBurstUntilMs[slotIdx] = nowMs + browserFavoriteSaveBurstDurationMs;
                    browserFavoriteMissingBurstUntilMs[slotIdx] = 0;
                }
                else
                {
                    browserFavoriteMissingBurstUntilMs[slotIdx] = nowMs + browserFavoriteMissingBurstDurationMs;
                }
            }
        }
    }

    if (controlModeActive && currentControlMode == ControlMode::Preset)
    {
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
                        presetPadSaveBurstUntilMs[idx] = nowMs + presetSaveBurstDurationMs;
                    }
                }

                const bool exists = presetExists(presetIndex);
                int level = exists ? 8 : 2;  // Existing lit, empty dim.
                const bool burstActive = nowMs < presetPadSaveBurstUntilMs[idx];
                if (burstActive)
                {
                    const bool burstOn = ((nowMs / presetSaveBurstIntervalMs) & 1u) == 0u;
                    level = burstOn ? 15 : 0;
                }
                else if (presetIndex == loadedPresetIndex && exists)
                {
                    level = slowBlinkOn ? 15 : 0;  // Loaded preset blinks.
                }
                newLedState[x][y] = level;
            }
        }

        // Keep control row visible while preset grid is active.
        for (int x = 0; x < NumControlRowPages && x < gridWidth; ++x)
            newLedState[x][CONTROL_ROW] = 5;
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages && activeButton < gridWidth)
            newLedState[activeButton][CONTROL_ROW] = 15;
        // Metronome pulse on control-row quantize button (row 7, col 15):
        // beat pulses dim, bar "1" pulses bright.
        if (metroPulseOn)
            newLedState[15][CONTROL_ROW] = metroDownbeat ? 15 : 7;
        else
            newLedState[15][CONTROL_ROW] = 5;

        for (int y = 0; y < gridHeight; ++y)
        {
            for (int x = 0; x < gridWidth; ++x)
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
    if (controlModeActive && currentControlMode == ControlMode::StepEdit)
    {
        for (int i = 0; i < 16; ++i)
            newLedState[i][GROUP_ROW] = 0;

        auto getStepToolColumn = [this]()
        {
            switch (stepEditTool)
            {
                case StepEditTool::Velocity: return 0;
                case StepEditTool::Divide: return 1;
                case StepEditTool::RampUp: return 2;
                case StepEditTool::RampDown: return 3;
                case StepEditTool::Probability: return 4;
                case StepEditTool::Attack: return 5;
                case StepEditTool::Decay: return 6;
                case StepEditTool::Release: return 7;
                case StepEditTool::Gate:
                default: return -1;
            }
        };

        const int toolColumn = getStepToolColumn();
        for (int col = 0; col <= 7; ++col)
            newLedState[col][GROUP_ROW] = (col == toolColumn) ? 15 : 4;

        stepEditSelectedStrip = clampVisibleStrip(stepEditSelectedStrip);
        const int selectedStripIndex = stepEditSelectedStrip;
        if (selectedStripIndex < (stepEditStripBank * stepEditBankSize)
            || selectedStripIndex >= ((stepEditStripBank + 1) * stepEditBankSize))
        {
            stepEditStripBank = juce::jlimit(0, maxStepEditBank, selectedStripIndex / stepEditBankSize);
        }
        const int bankStart = stepEditStripBank * stepEditBankSize;

        for (int col = 8; col <= 13; ++col)
        {
            const int stripIndex = bankStart + (col - 8);
            if (stripIndex >= visibleStripCount)
                continue;

            auto* strip = audioEngine->getStrip(stripIndex);
            const bool inStepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
            int level = inStepMode ? 6 : 3;
            if (stripIndex == selectedStripIndex)
                level = inStepMode ? 15 : 10;
            newLedState[col][GROUP_ROW] = level;
        }

        const bool bankDownAvailable = (stepEditStripBank > 0);
        const bool bankUpAvailable = (stepEditStripBank < maxStepEditBank);
        newLedState[14][GROUP_ROW] = bankDownAvailable ? 9 : 2;
        newLedState[15][GROUP_ROW] = bankUpAvailable ? 9 : 2;
    }
    else if (controlModeActive
        && currentControlMode != ControlMode::Normal
        && currentControlMode != ControlMode::Modulation
        && currentControlMode != ControlMode::Filter
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
        const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
        const auto seq = audioEngine->getModSequencerState(targetStrip);
        const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(targetStrip);
        const int playbackPage = juce::jlimit(
            0,
            ModernAudioEngine::MaxModBars - 1,
            activeGlobalStep / ModernAudioEngine::ModSteps);
        const int activeStep = (playbackPage == seq.editPage)
            ? (activeGlobalStep % ModernAudioEngine::ModSteps)
            : -1;
        const bool stripPlaying = audioEngine->getStrip(targetStrip) && audioEngine->getStrip(targetStrip)->isPlaying();
        const int displayRow = 0; // Top row is highest value in modulation mode.
        const int modulationBaseRow = seq.bipolar ? (modulationMaxRow / 2) : modulationMaxRow;

        auto valueToRow = [&](float v)
        {
            v = juce::jlimit(0.0f, 1.0f, v);
            if (seq.bipolar)
            {
                const float signedV = (v * 2.0f) - 1.0f;
                const float n = (signedV + 1.0f) * 0.5f;
                return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - n) * modulationMaxRow)));
            }
            return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - v) * modulationMaxRow)));
        };
        auto curveLevelForRow = [&](int row, bool isPoint)
        {
            // Value-encoded intensity: high values (top rows) are more solid,
            // low values (bottom rows) are dimmer.
            const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
            const int base = juce::jlimit(1, 15, static_cast<int>(std::round(juce::jmap(rowValue01, 2.0f, 12.0f))));
            return juce::jlimit(1, 15, isPoint ? (base + 2) : base);
        };
        auto stepLevelForRow = [&](int row, int pointRow, int baseRow)
        {
            const int minRow = juce::jmin(baseRow, pointRow);
            const int maxRow = juce::jmax(baseRow, pointRow);
            if (row < minRow || row > maxRow)
                return 0;

            const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
            const float fromBase = static_cast<float>(std::abs(row - baseRow));
            const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f; // 0 at base, 1 at point
            const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);

            const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
            const float minLevel = seq.bipolar ? 3.0f : 2.0f;
            const float maxLevel = 9.0f + (4.0f * rowValue01); // 9..13, brighter for higher values
            int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
            if (row == pointRow)
                level += 2;
            return juce::jlimit(1, 15, level);
        };

        for (int x = 0; x < 16; ++x)
        {
            newLedState[x][GROUP_ROW] = 0;
            const float v = seq.steps[static_cast<size_t>(x)];
            const int pointRow = valueToRow(v);

            if (seq.curveMode)
            {
                int level = 0;
                if (displayRow == pointRow)
                    level = juce::jmax(level, curveLevelForRow(displayRow, true));
                if (x < 15)
                {
                    const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)]);
                    const int minRow = juce::jmin(pointRow, nextRow);
                    const int maxRow = juce::jmax(pointRow, nextRow);
                    if (displayRow >= minRow && displayRow <= maxRow)
                        level = juce::jmax(level, curveLevelForRow(displayRow, false));
                }
                newLedState[x][GROUP_ROW] = level;
            }
            else
            {
                newLedState[x][GROUP_ROW] = stepLevelForRow(displayRow, pointRow, modulationBaseRow);
            }

            if (stripPlaying && x == activeStep)
                newLedState[x][GROUP_ROW] = juce::jmax(newLedState[x][GROUP_ROW], 15);
        }

        const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, seq.lengthBars);
        const int editPage = juce::jlimit(0, lengthBars - 1, seq.editPage);
        newLedState[11][GROUP_ROW] = (editPage > 0) ? 10 : 2;                 // Page down
        newLedState[12][GROUP_ROW] = (editPage < (lengthBars - 1)) ? 10 : 2;  // Page up

        const int activeSlot = audioEngine->getModSequencerSlot(targetStrip);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            const int col = 13 + slot;
            newLedState[col][GROUP_ROW] = (slot == activeSlot) ? 15 : 4;
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

    // Row 0, cols 9-15: momentary stutter division selectors.
    // Visible on normal page only.
    if (!controlModeActive || currentControlMode == ControlMode::Normal)
    {
        const uint8_t heldMask = momentaryStutterButtonMask.load(std::memory_order_acquire);
        for (int x = 9; x <= 15; ++x)
        {
            const uint8_t bit = stutterButtonBitForColumn(x);
            const bool held = (heldMask & bit) != 0;
            const bool active = momentaryStutterHoldActive && (momentaryStutterActiveDivisionButton == x);
            if (active)
                newLedState[x][GROUP_ROW] = fastBlinkOn ? 15 : 8;
            else if (held)
                newLedState[x][GROUP_ROW] = 9;
            else
                newLedState[x][GROUP_ROW] = 2;
        }
    }
    
    // Strip rows (between group row and control row).
    for (int stripIndex = 0; stripIndex < visibleStripCount; ++stripIndex)
    {
        int y = FIRST_STRIP_ROW + stripIndex;
        auto* strip = audioEngine->getStrip(stripIndex);

        if (controlModeActive && currentControlMode == ControlMode::StepEdit)
        {
            const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
            auto* selectedStrip = audioEngine->getStrip(selectedStripIndex);
            if (!selectedStrip)
            {
                for (int x = 0; x < 16; ++x)
                    newLedState[x][y] = 0;
                continue;
            }

            const int totalSteps = selectedStrip->getStepTotalSteps();
            const int visibleOffset = selectedStrip->getVisibleStepOffset();
            const int visibleCurrentStep = selectedStrip->getVisibleCurrentStep();
            const bool stripPlaying = selectedStrip->isPlaying()
                && selectedStrip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
            const float rowNorm = stepEditRowNorm(y);

            if (stepEditTool == StepEditTool::Attack
                || stepEditTool == StepEditTool::Decay
                || stepEditTool == StepEditTool::Release)
            {
                float normalized = 0.0f;
                if (stepEditTool == StepEditTool::Attack)
                    normalized = juce::jlimit(0.0f, 1.0f, selectedStrip->getStepEnvelopeAttackMs() / 400.0f);
                else if (stepEditTool == StepEditTool::Decay)
                    normalized = juce::jlimit(0.0f, 1.0f, (selectedStrip->getStepEnvelopeDecayMs() - 1.0f) / 3999.0f);
                else
                {
                    const float pitchSemitones = getPitchSemitonesForDisplay(*selectedStrip);
                    normalized = juce::jlimit(0.0f, 1.0f, (pitchSemitones + 24.0f) / 48.0f);
                }

                const int activeCol = juce::jlimit(0, 15, static_cast<int>(std::round(normalized * 15.0f)));
                for (int x = 0; x < 16; ++x)
                {
                    int level = (x == activeCol) ? 15 : ((x < activeCol) ? 6 : 1);
                    if (stripPlaying && x == visibleCurrentStep)
                        level = juce::jmax(level, 9);
                    newLedState[x][y] = level;
                }
                continue;
            }

            for (int x = 0; x < 16; ++x)
            {
                const int absoluteStep = visibleOffset + x;
                if (absoluteStep < 0 || absoluteStep >= totalSteps)
                {
                    newLedState[x][y] = 0;
                    continue;
                }

                const auto idx = static_cast<size_t>(absoluteStep);
                const bool enabled = selectedStrip->stepPattern[idx];
                const int subdivision = selectedStrip->getStepSubdivisionAtIndex(absoluteStep);
                const float startVelocity = selectedStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                const float endVelocity = selectedStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                const float maxVelocity = juce::jmax(startVelocity, endVelocity);
                const float probability = selectedStrip->getStepProbabilityAtIndex(absoluteStep);

                int level = 0;
                if (stepEditTool == StepEditTool::Gate)
                {
                    level = (enabled && y == (CONTROL_ROW - 1)) ? 12 : 0;
                }
                else
                {
                    float value = 0.0f;
                    switch (stepEditTool)
                    {
                        case StepEditTool::Gate:
                        case StepEditTool::Attack:
                        case StepEditTool::Decay:
                        case StepEditTool::Release:
                            break;
                        case StepEditTool::Velocity:
                            value = enabled ? maxVelocity : 0.0f;
                            break;
                        case StepEditTool::Divide:
                            value = enabled
                                ? static_cast<float>(subdivision - 1)
                                    / static_cast<float>(juce::jmax(1, EnhancedAudioStrip::MaxStepSubdivisions - 1))
                                : 0.0f;
                            break;
                        case StepEditTool::RampUp:
                        {
                            const float base = juce::jmax(0.001f, maxVelocity);
                            value = enabled ? juce::jlimit(0.0f, 1.0f, 1.0f - (startVelocity / base)) : 0.0f;
                            break;
                        }
                        case StepEditTool::RampDown:
                        {
                            const float base = juce::jmax(0.001f, maxVelocity);
                            value = enabled ? juce::jlimit(0.0f, 1.0f, 1.0f - (endVelocity / base)) : 0.0f;
                            break;
                        }
                        case StepEditTool::Probability:
                            value = enabled ? probability : 0.0f;
                            break;
                    }

                    if (value + 0.0001f >= rowNorm)
                        level = enabled ? 11 : 7;
                    else
                        level = enabled ? 2 : 0;
                }

                if (stripPlaying && x == visibleCurrentStep)
                    level = juce::jmax(level, (y == (CONTROL_ROW - 1)) ? 15 : 6);
                newLedState[x][y] = level;
            }

            continue;
        }
        
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
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Pitch)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Pan)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Volume)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Swing)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Gate)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::GrainSize)
        {
            const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                MonomeMixActions::renderGrainPageRow(*targetStrip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Filter)
        {
            MonomeFilterActions::renderRow(*strip, y, newLedState, static_cast<int>(filterSubPage));
        }
        else if (controlModeActive && currentControlMode == ControlMode::FileBrowser)
        {
            MonomeFileBrowserActions::renderRow(*this, *audioEngine, *strip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::GroupAssign)
        {
            MonomeGroupAssignActions::renderRow(*strip, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Modulation)
        {
            const int selectedStrip = clampVisibleStrip(getLastMonomePressedStripRow());
            const auto seq = audioEngine->getModSequencerState(selectedStrip);
            const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(selectedStrip);
            const int playbackPage = juce::jlimit(
                0,
                ModernAudioEngine::MaxModBars - 1,
                activeGlobalStep / ModernAudioEngine::ModSteps);
            const int activeStep = (playbackPage == seq.editPage)
                ? (activeGlobalStep % ModernAudioEngine::ModSteps)
                : -1;
            const bool stripPlaying = audioEngine->getStrip(selectedStrip) && audioEngine->getStrip(selectedStrip)->isPlaying();
            const int displayRow = y; // Strip rows, with row 0 rendered in GROUP_ROW branch.
            const int modulationBaseRow = seq.bipolar ? (modulationMaxRow / 2) : modulationMaxRow;

            auto valueToRow = [&](float v)
            {
                v = juce::jlimit(0.0f, 1.0f, v);
                if (seq.bipolar)
                {
                    const float signedV = (v * 2.0f) - 1.0f;
                    const float n = (signedV + 1.0f) * 0.5f;
                    return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - n) * modulationMaxRow)));
                }
                return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - v) * modulationMaxRow)));
            };
            auto curveLevelForRow = [&](int row, bool isPoint)
            {
                // Value-encoded intensity: high values (top rows) are more solid,
                // low values (bottom rows) are dimmer.
                const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                const int base = juce::jlimit(1, 15, static_cast<int>(std::round(juce::jmap(rowValue01, 2.0f, 12.0f))));
                return juce::jlimit(1, 15, isPoint ? (base + 2) : base);
            };
            auto stepLevelForRow = [&](int row, int pointRow, int baseRow)
            {
                const int minRow = juce::jmin(baseRow, pointRow);
                const int maxRow = juce::jmax(baseRow, pointRow);
                if (row < minRow || row > maxRow)
                    return 0;

                const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
                const float fromBase = static_cast<float>(std::abs(row - baseRow));
                const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f; // 0 at base, 1 at point
                const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);

                const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                const float minLevel = seq.bipolar ? 3.0f : 2.0f;
                const float maxLevel = 9.0f + (4.0f * rowValue01); // 9..13, brighter for higher values
                int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
                if (row == pointRow)
                    level += 2;
                return juce::jlimit(1, 15, level);
            };

            for (int x = 0; x < 16; ++x)
            {
                newLedState[x][y] = 0;
                const float v = seq.steps[static_cast<size_t>(x)];
                const int pointRow = valueToRow(v);

                if (seq.curveMode)
                {
                    // Draw point + interpolated line to next point for readable curve graph.
                    int level = 0;
                    if (displayRow == pointRow)
                        level = juce::jmax(level, curveLevelForRow(displayRow, true));
                    if (x < 15)
                    {
                        const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)]);
                        const int minRow = juce::jmin(pointRow, nextRow);
                        const int maxRow = juce::jmax(pointRow, nextRow);
                        if (displayRow >= minRow && displayRow <= maxRow)
                            level = juce::jmax(level, curveLevelForRow(displayRow, false));
                    }
                    newLedState[x][y] = level;
                }
                else
                {
                    // Step-slider mode: vertical bar to value.
                    newLedState[x][y] = stepLevelForRow(displayRow, pointRow, modulationBaseRow);
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
                // NORMAL PLAYBACK MODE - show playhead with fractional interpolation.
                const int loopStart = juce::jlimit(0, 15, strip->getLoopStart());
                const int loopEnd = juce::jlimit(loopStart + 1, 16, strip->getLoopEnd());
                for (int x = loopStart; x < loopEnd && x < 16; ++x)
                    newLedState[x][y] = 2;

                const int loopCols = juce::jmax(1, loopEnd - loopStart);
                const int currentCol = strip->getCurrentColumn();
                bool drewInterpolatedPlayhead = false;

                const auto* audioBuffer = strip->getAudioBuffer();
                const int sampleCount = (audioBuffer != nullptr) ? audioBuffer->getNumSamples() : 0;
                const double playbackPos = strip->getPlaybackPosition();
                if (sampleCount > 0 && std::isfinite(playbackPos))
                {
                    double normalized = playbackPos / static_cast<double>(sampleCount);
                    normalized = std::fmod(normalized, 1.0);
                    if (normalized < 0.0)
                        normalized += 1.0;

                    double loopPosCols = (normalized * static_cast<double>(MaxColumns))
                        - static_cast<double>(loopStart);
                    loopPosCols = std::fmod(loopPosCols, static_cast<double>(loopCols));
                    if (loopPosCols < 0.0)
                        loopPosCols += static_cast<double>(loopCols);

                    const int baseOffset = juce::jlimit(
                        0, loopCols - 1, static_cast<int>(std::floor(loopPosCols)));
                    const double frac = juce::jlimit(
                        0.0, 1.0, loopPosCols - static_cast<double>(baseOffset));
                    const int baseCol = loopStart + baseOffset;
                    const int nextCol = loopStart + ((baseOffset + 1) % loopCols);
                    const int baseLevel = juce::jlimit(
                        2, 15, static_cast<int>(std::lround(15.0 - (frac * 7.0))));
                    const int nextLevel = juce::jlimit(
                        2, 15, static_cast<int>(std::lround(8.0 + (frac * 7.0))));

                    if (baseCol >= 0 && baseCol < 16)
                        newLedState[baseCol][y] = juce::jmax(newLedState[baseCol][y], baseLevel);
                    if (nextCol >= 0 && nextCol < 16)
                        newLedState[nextCol][y] = juce::jmax(newLedState[nextCol][y], nextLevel);

                    drewInterpolatedPlayhead = true;
                }

                if (!drewInterpolatedPlayhead && currentCol >= 0 && currentCol < 16)
                    newLedState[currentCol][y] = 15;
            }
        }
    }
    
    // Control row (bottom row): page buttons and quantize indicator.
    for (int x = 0; x < NumControlRowPages && x < gridWidth; ++x)
        newLedState[x][CONTROL_ROW] = 5;

    if (controlModeActive)
    {
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages && activeButton < gridWidth)
            newLedState[activeButton][CONTROL_ROW] = 15;
    }

    if (controlModeActive && currentControlMode == ControlMode::StepEdit)
    {
        const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
        bool hasSelectedStrip = false;
        int pitchSemitones = 0;

        if (auto* selectedStrip = audioEngine->getStrip(selectedStripIndex))
        {
            hasSelectedStrip = true;
            pitchSemitones = static_cast<int>(std::round(selectedStrip->getPitchShift()));

            if (selectedStrip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                pitchSemitones = static_cast<int>(std::round(getPitchSemitonesForDisplay(*selectedStrip)));
            }
        }

        const bool canDown = hasSelectedStrip && pitchSemitones > -24;
        const bool canUp = hasSelectedStrip && pitchSemitones < 24;

        int downLevel = canDown ? 8 : 2;
        int upLevel = canUp ? 8 : 2;
        if (pitchSemitones < 0)
            downLevel = canDown ? 13 : 3;
        else if (pitchSemitones > 0)
            upLevel = canUp ? 13 : 3;
        else if (hasSelectedStrip)
        {
            downLevel = canDown ? 9 : 2;
            upLevel = canUp ? 9 : 2;
        }

        newLedState[13][CONTROL_ROW] = downLevel;
        newLedState[14][CONTROL_ROW] = upLevel;
    }

    // Metronome pulse on control-row quantize button (row 7, col 15):
    // beat pulses dim, bar "1" pulses bright.
    if (metroPulseOn)
        newLedState[15][CONTROL_ROW] = metroDownbeat ? 15 : 7;
    else
        newLedState[15][CONTROL_ROW] = 5;
    
    // Differential update
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
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
