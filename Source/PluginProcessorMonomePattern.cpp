/*
  ==============================================================================

    PluginProcessorMonomePattern.cpp
    Extracted monome pattern recorder control playback helpers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "MonomeMixActions.h"
#include "PlayheadSpeedQuantizer.h"
#include <array>
#include <cmath>

void MlrVSTAudioProcessor::recordMonomeControlPatternEvent(ControlMode mode,
                                                           int targetStripIndex,
                                                           int controlRow,
                                                           int column)
{
    if (!audioEngine || !monomeControlPageShowsPatternRecorder(mode))
        return;

    if (isSceneModeEnabled())
    {
        recordMonomeControlSceneEvent(mode, targetStripIndex, controlRow, column);
        return;
    }

    // Normal mode: capture control rides into any recording pattern, mirroring
    // the trigger-recording path (PluginProcessor.cpp stopStrip-area loop).
    // Playback filters by patternRecorderMatchesStrip as well, so recording
    // uses the same strip-match rule for symmetric behavior.
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, targetStripIndex);
    const double eventBeat = audioEngine->getTimelineBeat();
    if (!std::isfinite(eventBeat))
        return;

    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        auto* pattern = audioEngine->getPattern(patternIndex);
        if (pattern != nullptr
            && pattern->isRecording()
            && audioEngine->patternRecorderMatchesStrip(patternIndex, safeStripIndex))
        {
            pattern->recordControlEvent(safeStripIndex,
                                        static_cast<int>(mode),
                                        controlRow,
                                        column,
                                        eventBeat);
        }
    }
}

void MlrVSTAudioProcessor::playbackMonomeControlPatternEvent(const PatternRecorder::Event& event)
{
    if (!audioEngine)
        return;

    const int stripIndex = juce::jlimit(0, MaxStrips - 1, event.stripIndex);
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto mode = static_cast<ControlMode>(event.controlMode);
    const int controlRow = event.controlRow;
    const int column = juce::jlimit(0, MaxColumns - 1, event.column);

    switch (mode)
    {
        case ControlMode::Speed:
        {
            setStripSpeedControlValue(stripIndex,
                                      PlayheadSpeedQuantizer::monomeSpeedControlValueFromColumn(
                                          column,
                                          strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain),
                                      StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Pan:
        {
            // Same piecewise map as the live handler so replayed automation
            // matches what was performed (column 15 reaches full right).
            const float pan = juce::jlimit(-1.0f,
                                           1.0f,
                                           column <= 8 ? (column - 8) / 8.0f
                                                       : (column - 8) / 7.0f);
            setStripPanControlValue(stripIndex,
                                    pan,
                                    StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Volume:
        {
            setStripVolumeControlValue(stripIndex,
                                       juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                       StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Swing:
        case ControlMode::Gate:
        {
            MonomeMixActions::applyButtonPressLive(*strip,
                                                   column,
                                                   static_cast<int>(mode),
                                                   static_cast<int>(getGatePageMode()));
            break;
        }

        case ControlMode::Pitch:
        {
            static constexpr std::array<float, 16> kMusicalPitchSemitones{
                -24.0f, -21.0f, -18.0f, -15.0f, -12.0f, -9.0f, -6.0f, -3.0f,
                  0.0f,   3.0f,   6.0f,   9.0f,  12.0f, 15.0f, 18.0f, 24.0f
            };
            const float quantizedSemitones = quantizePitchSemitonesForStripControl(
                stripIndex,
                kMusicalPitchSemitones[static_cast<size_t>(column)]);
            if (auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
                rawParam->store(quantizedSemitones, std::memory_order_release);

            const auto loopPitchRole = getLoopPitchRole(stripIndex);
            if (loopPitchRole == LoopPitchRole::Sync)
                requestLoopPitchRoleStateUpdate(stripIndex);
            else
                applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);

            if (loopPitchRole == LoopPitchRole::Master)
                updateGlobalRootFromLoopPitchMaster(stripIndex, true);
            break;
        }

        case ControlMode::GrainSize:
            MonomeMixActions::applyGrainPageButtonPressLive(*strip, controlRow, column);
            break;

        case ControlMode::Delay:
        {
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    setStripDelayMixControlValue(stripIndex,
                                                 juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                                 StripControlWriteMode::CacheOnly);
                    break;
                case 1:
                {
                    const bool syncEnabled = strip->isDelaySyncEnabled();
                    const juce::NormalisableRange<float> syncRange(0.25f, 4.0f, 0.001f, 0.45f);
                    const juce::NormalisableRange<float> freeRange(0.25f, 4.0f, 0.001f, 0.4f);
                    const float unit = juce::jlimit(0.0f, 1.0f, column / 15.0f);
                    const float timeValue = (syncEnabled ? syncRange : freeRange).convertFrom0to1(unit);
                    setStripDelayTimeControlValue(stripIndex,
                                                  timeValue,
                                                  StripControlWriteMode::CacheOnly);
                    break;
                }
                case 2:
                    setStripDelayFeedbackControlValue(stripIndex,
                                                      0.97f * juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                                      StripControlWriteMode::CacheOnly);
                    break;
                case 3:
                {
                    const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
                    setStripDelayLowCutControlValue(stripIndex,
                                                    range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, column / 15.0f)),
                                                    StripControlWriteMode::CacheOnly);
                    break;
                }
                case 4:
                {
                    const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
                    setStripDelayHighCutControlValue(stripIndex,
                                                     range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, column / 15.0f)),
                                                     StripControlWriteMode::CacheOnly);
                    break;
                }
                case 5:
                    if (column <= 2)
                    {
                        setStripDelayModeControlValue(stripIndex,
                                                      static_cast<EnhancedAudioStrip::DelayMode>(column),
                                                      StripControlWriteMode::CacheOnly);
                    }
                    else if (column >= 12)
                    {
                        setStripDelaySyncEnabledControlValue(stripIndex,
                                                             column >= 14,
                                                             StripControlWriteMode::CacheOnly);
                    }
                    break;
                default:
                    break;
            }
            break;
        }

        case ControlMode::Filter:
            if (controlRow == 0)
            {
                const float t = juce::jlimit(0.0f, 1.0f, column / 15.0f);
                setStripFilterFrequencyControlValue(stripIndex,
                                                    20.0f * std::pow(1000.0f, t),
                                                    StripControlWriteMode::CacheOnly,
                                                    true);
            }
            else if (controlRow == 1)
            {
                setStripFilterResonanceControlValue(stripIndex,
                                                    0.1f + (juce::jlimit(0.0f, 15.0f, static_cast<float>(column)) / 15.0f) * 9.9f,
                                                    StripControlWriteMode::CacheOnly);
            }
            else if (controlRow == 2)
            {
                if (column <= 2)
                {
                    const float morph = (column <= 0) ? 0.0f : (column == 1 ? 0.5f : 1.0f);
                    setStripFilterMorphControlValue(stripIndex,
                                                    morph,
                                                    StripControlWriteMode::CacheOnly);
                }
            }
            break;

        case ControlMode::Normal:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            break;
    }
}
