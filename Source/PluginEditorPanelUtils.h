#pragma once

#include "PluginProcessor.h"
#include "PluginEditorStyle.h"
#include "PlayheadSpeedQuantizer.h"
#include <cmath>

namespace PluginEditorPanelUtils
{
// Shared color language for recorded control events (pattern cards, scene
// automation lanes, monome page tints). Fallback is the neutral data color —
// amber stays reserved for selection/focus.
inline juce::Colour controlModeEventColour(int controlMode)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (static_cast<ControlMode>(controlMode))
    {
        case ControlMode::Speed:     return juce::Colour(0xff6bbcff);
        case ControlMode::Pitch:     return juce::Colour(0xffff8f6b);
        case ControlMode::Pan:       return juce::Colour(0xff6ce0c4);
        case ControlMode::Volume:    return juce::Colour(0xff88d96b);
        case ControlMode::GrainSize: return juce::Colour(0xffffc86b);
        case ControlMode::Swing:     return juce::Colour(0xffffdd74);
        case ControlMode::Gate:      return juce::Colour(0xffff7d9c);
        case ControlMode::Delay:     return juce::Colour(0xff74d6ff);
        case ControlMode::Filter:    return juce::Colour(0xffff9d5a);
        case ControlMode::Normal:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:                     return PluginEditorStyle::kDataNeutral;
    }
}
inline juce::String getMonomePageDisplayName(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Normal: return "Normal";
        case MlrVSTAudioProcessor::ControlMode::Speed: return "Speed";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "Pitch";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "Pan";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "Volume";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "Grain Size";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "Filter";
        case MlrVSTAudioProcessor::ControlMode::Delay: return "Delay";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "Swing";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "Gate";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "Modulation";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "Preset Loader";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "Step Edit";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "Group Assign";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "File Browser";
    }
    return "Normal";
}

inline juce::String getMonomePageShortName(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Speed: return "SPD";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "PIT";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "PAN";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "VOL";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "GRN";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "FLT";
        case MlrVSTAudioProcessor::ControlMode::Delay: return "DLY";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "SWG";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "GATE";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "BRW";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "GRP";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "MOD";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "PST";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "STEP";
        case MlrVSTAudioProcessor::ControlMode::Normal:
        default: return "NORM";
    }
}

inline int pitchScaleToComboId(ModernAudioEngine::PitchScale scale)
{
    switch (scale)
    {
        case ModernAudioEngine::PitchScale::Chromatic: return 1;
        case ModernAudioEngine::PitchScale::Major: return 2;
        case ModernAudioEngine::PitchScale::Minor: return 3;
        case ModernAudioEngine::PitchScale::Dorian: return 4;
        case ModernAudioEngine::PitchScale::PentatonicMinor: return 5;
        default: return 1;
    }
}

inline ModernAudioEngine::PitchScale comboIdToPitchScale(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::PitchScale::Major;
        case 3: return ModernAudioEngine::PitchScale::Minor;
        case 4: return ModernAudioEngine::PitchScale::Dorian;
        case 5: return ModernAudioEngine::PitchScale::PentatonicMinor;
        case 1:
        default: return ModernAudioEngine::PitchScale::Chromatic;
    }
}

inline juce::String getPitchClassName(int pitchClass)
{
    static constexpr const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B" };
    return juce::String(names[((pitchClass % 12) + 12) % 12]);
}

inline juce::String makePresetBubbleLabel(const juce::String& name, int fallbackIndex)
{
    auto trimmed = name.trim();
    if (trimmed.isEmpty())
        return juce::String(fallbackIndex + 1);

    juce::StringArray tokens;
    tokens.addTokens(trimmed, " _-/", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    auto isGenericToken = [](const juce::String& token)
    {
        const auto upper = token.trim().toUpperCase();
        return upper == "PRESET" || upper == "PATCH";
    };

    juce::StringArray filteredTokens;
    for (const auto& token : tokens)
    {
        if (!isGenericToken(token))
            filteredTokens.add(token);
    }

    juce::String compact;
    if (filteredTokens.size() > 1)
    {
        for (const auto& token : filteredTokens)
        {
            for (auto c : token)
            {
                if (juce::CharacterFunctions::isLetterOrDigit(c))
                {
                    compact << juce::String::charToString(juce::CharacterFunctions::toUpperCase(c));
                    break;
                }
            }

            if (compact.length() >= 4)
                break;
        }
    }
    else
    {
        const auto source = filteredTokens.isEmpty() ? trimmed : filteredTokens[0];
        for (auto c : source)
        {
            if (juce::CharacterFunctions::isLetterOrDigit(c))
                compact << juce::String::charToString(juce::CharacterFunctions::toUpperCase(c));
            if (compact.length() >= 4)
                break;
        }
    }

    if (compact.isEmpty())
        compact = juce::String(fallbackIndex + 1);

    return compact;
}

inline const juce::NormalisableRange<float>& macroCutoffDisplayRange()
{
    static const juce::NormalisableRange<float> range = []
    {
        juce::NormalisableRange<float> r(20.0f, 20000.0f, 1.0f);
        r.setSkewForCentre(1000.0f);
        return r;
    }();
    return range;
}

inline float macroCutoffHzFromNormalized(float normalized)
{
    return juce::jlimit(20.0f,
                        20000.0f,
                        macroCutoffDisplayRange().convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalized)));
}

inline juce::String macroCutoffText(float normalized)
{
    const float hz = macroCutoffHzFromNormalized(normalized);
    if (hz >= 1000.0f)
        return juce::String(hz / 1000.0f, 2) + " kHz";
    return juce::String(static_cast<int>(std::round(hz))) + " Hz";
}

inline juce::String macroFrequencyText(float hz)
{
    if (hz >= 1000.0f)
        return juce::String(hz / 1000.0f, 2) + " kHz";
    return juce::String(static_cast<int>(std::round(hz))) + " Hz";
}

inline juce::String macroResonanceText(float normalized)
{
    const float q = juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, 0.1f, 10.0f);
    return "Q " + juce::String(q, 2);
}

inline juce::String macroMorphText(float normalized)
{
    const float v = juce::jlimit(0.0f, 1.0f, normalized);
    if (v < 0.25f) return "LP";
    if (v < 0.75f) return "BP";
    return "HP";
}

inline juce::String macroPitchText(float normalized)
{
    const float semitones = juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, -24.0f, 24.0f);
    return juce::String(semitones, 1) + " st";
}

inline juce::String macroDelayTimeText(float normalized)
{
    const float beats = juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, 0.25f, 4.0f);
    return juce::String(beats, beats < 1.0f ? 2 : 2) + " beats";
}

inline int macroTargetToComboId(MlrVSTAudioProcessor::MacroTarget target)
{
    return performanceTargetToComboId(sanitizeMacroPerformanceTarget(target));
}

inline MlrVSTAudioProcessor::MacroTarget comboIdToMacroTargetSelection(int comboId)
{
    return sanitizeMacroPerformanceTarget(performanceTargetFromComboId(comboId));
}

inline juce::String macroTargetDisplayName(MlrVSTAudioProcessor::MacroTarget target)
{
    return performanceTargetDisplayName(sanitizeMacroPerformanceTarget(target));
}

inline void populateMacroTargetBox(juce::ComboBox& box)
{
    for (auto target : kMacroPerformanceTargetOrder)
        box.addItem(macroTargetDisplayName(target), performanceTargetToComboId(target));
}

inline juce::String macroCcLabelText(int ccNumber)
{
    return ccNumber >= 0 ? ("CC" + juce::String(ccNumber)) : "CC--";
}

inline juce::String retriggerDivisionLabel(float amount01)
{
    const float v = juce::jlimit(0.0f, 1.0f, amount01);
    if (v <= 1.0e-4f) return "Off";
    if (v < 0.125f) return "1/2";
    if (v < 0.250f) return "1/4";
    if (v < 0.375f) return "1/8";
    if (v < 0.500f) return "1/16";
    if (v < 0.750f) return "1/32";
    return "1/64";
}

inline juce::String macroValueText(MlrVSTAudioProcessor::MacroTarget target, float normalized)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, normalized);

    switch (target)
    {
        case MlrVSTAudioProcessor::MacroTarget::Cutoff:
            return macroCutoffText(clamped);
        case MlrVSTAudioProcessor::MacroTarget::Resonance:
            return macroResonanceText(clamped);
        case MlrVSTAudioProcessor::MacroTarget::FilterMorph:
            return macroMorphText(clamped);
        case MlrVSTAudioProcessor::MacroTarget::Pitch:
            return macroPitchText(clamped);
        case MlrVSTAudioProcessor::MacroTarget::Volume:
            return juce::String(static_cast<int>(std::round(clamped * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::Pan:
        {
            const float pan = juce::jmap(clamped, 0.0f, 1.0f, -1.0f, 1.0f);
            if (std::abs(pan) < 0.01f)
                return "C";
            return juce::String(pan < 0.0f ? "L" : "R")
                + juce::String(static_cast<int>(std::round(std::abs(pan) * 100.0f)));
        }
        case MlrVSTAudioProcessor::MacroTarget::FilterEnable:
            return clamped >= 0.5f ? "On" : "Off";
        case MlrVSTAudioProcessor::MacroTarget::Speed:
            return juce::String(PlayheadSpeedQuantizer::labelForRatio(juce::jmap(clamped, 0.0f, 1.0f, 0.125f, 8.0f)));
        case MlrVSTAudioProcessor::MacroTarget::SliceLength:
            return juce::String(static_cast<int>(std::round(clamped * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::Scratch:
            return juce::String(static_cast<int>(std::round(clamped * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::GrainSize:
            return juce::String(juce::jmap(clamped, 0.0f, 1.0f, 5.0f, 2400.0f), 1) + " ms";
        case MlrVSTAudioProcessor::MacroTarget::GrainDensity:
            return juce::String(static_cast<int>(std::round(juce::jmap(clamped, 0.0f, 1.0f, 0.05f, 0.9f) * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::GrainPitch:
            return juce::String(juce::jmap(clamped, 0.0f, 1.0f, -48.0f, 48.0f), 1) + " st";
        case MlrVSTAudioProcessor::MacroTarget::GrainPitchJitter:
            return juce::String(juce::jmap(clamped, 0.0f, 1.0f, 0.0f, 48.0f), 1) + " st";
        case MlrVSTAudioProcessor::MacroTarget::GrainSpread:
        case MlrVSTAudioProcessor::MacroTarget::GrainJitter:
        case MlrVSTAudioProcessor::MacroTarget::GrainPositionJitter:
        case MlrVSTAudioProcessor::MacroTarget::GrainRandom:
        case MlrVSTAudioProcessor::MacroTarget::GrainArp:
        case MlrVSTAudioProcessor::MacroTarget::GrainCloud:
        case MlrVSTAudioProcessor::MacroTarget::GrainEmitter:
        case MlrVSTAudioProcessor::MacroTarget::GrainEnvelope:
            return juce::String(static_cast<int>(std::round(clamped * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::GrainShape:
            return juce::String(juce::jmap(clamped, 0.0f, 1.0f, -1.0f, 1.0f) * 100.0f, 0) + "%";
        case MlrVSTAudioProcessor::MacroTarget::Retrigger:
            return retriggerDivisionLabel(clamped);
        case MlrVSTAudioProcessor::MacroTarget::Rearrange:
            return "Unassigned";
        case MlrVSTAudioProcessor::MacroTarget::DelayMix:
            return juce::String(static_cast<int>(std::round(clamped * 100.0f))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::DelayTime:
            return macroDelayTimeText(clamped);
        case MlrVSTAudioProcessor::MacroTarget::DelayFeedback:
            return juce::String(static_cast<int>(std::round(juce::jmap(clamped, 0.0f, 1.0f, 0.0f, 97.0f)))) + "%";
        case MlrVSTAudioProcessor::MacroTarget::DelayLowCut:
            return macroFrequencyText(juce::jmap(clamped, 0.0f, 1.0f, 20.0f, 12000.0f));
        case MlrVSTAudioProcessor::MacroTarget::DelayHighCut:
            return macroFrequencyText(juce::jmap(clamped, 0.0f, 1.0f, 200.0f, 20000.0f));
        case MlrVSTAudioProcessor::MacroTarget::None:
        default:
            return "Unassigned";
    }
}
} // namespace PluginEditorPanelUtils
