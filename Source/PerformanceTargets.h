#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstddef>

enum class PerformanceTarget
{
    None = 0,
    Volume,
    Pan,
    Pitch,
    Speed,
    Cutoff,
    Resonance,
    GrainSize,
    GrainDensity,
    GrainPitch,
    GrainPitchJitter,
    GrainSpread,
    GrainJitter,
    GrainRandom,
    GrainArp,
    GrainCloud,
    GrainEmitter,
    GrainEnvelope,
    Retrigger,
    GrainPositionJitter,
    GrainShape,
    FilterMorph,
    FilterEnable,
    SliceLength,
    Scratch,
    FilterFrequency = Cutoff
};

struct PerformanceTargetInfo
{
    PerformanceTarget target;
    const char* key;
    const char* displayName;
    const char* compactDisplayName;
    bool macroAllowed;
    bool modLaneAllowed;
    bool supportsBipolar;
    bool supportsPitchScaleQuantize;
};

inline constexpr std::array<PerformanceTargetInfo, 25> kPerformanceTargetInfos{{
    { PerformanceTarget::None, "none", "None", "None", true, true, false, false },
    { PerformanceTarget::Volume, "volume", "Volume", "Vol", true, true, false, false },
    { PerformanceTarget::Pan, "pan", "Pan", "Pan", true, true, true, false },
    { PerformanceTarget::Pitch, "pitch", "Pitch", "Pitch", true, true, true, true },
    { PerformanceTarget::Speed, "speed", "Speed", "Speed", true, true, false, false },
    { PerformanceTarget::Cutoff, "cutoff", "Cutoff", "Cutoff", true, true, false, false },
    { PerformanceTarget::Resonance, "resonance", "Resonance", "Reso", true, true, false, false },
    { PerformanceTarget::GrainSize, "grain_size", "Grain Size", "G.Size", true, true, true, false },
    { PerformanceTarget::GrainDensity, "grain_density", "Grain Density", "G.Dens", true, true, true, false },
    { PerformanceTarget::GrainPitch, "grain_pitch", "Grain Pitch", "G.Pitch", true, true, true, true },
    { PerformanceTarget::GrainPitchJitter, "grain_pitch_jitter", "Grain Pitch Jitter", "G.PJit", true, true, false, false },
    { PerformanceTarget::GrainSpread, "grain_spread", "Grain Spread", "G.Spread", true, true, true, false },
    { PerformanceTarget::GrainJitter, "grain_jitter", "Grain Jitter", "G.Jitter", true, true, false, false },
    { PerformanceTarget::GrainRandom, "grain_random", "Grain Random", "G.Random", true, true, false, false },
    { PerformanceTarget::GrainArp, "grain_arp", "Grain Arp", "G.Arp", true, true, false, false },
    { PerformanceTarget::GrainCloud, "grain_cloud", "Grain Cloud", "G.Cloud", true, true, false, false },
    { PerformanceTarget::GrainEmitter, "grain_emitter", "Grain Emitter", "G.Emit", true, true, false, false },
    { PerformanceTarget::GrainEnvelope, "grain_envelope", "Grain Envelope", "G.Env", true, true, false, false },
    { PerformanceTarget::Retrigger, "retrigger", "Stutter", "Retrig", false, true, false, false },
    { PerformanceTarget::GrainPositionJitter, "grain_position_jitter", "Grain Pos Jitter", "G.PosJ", true, true, true, false },
    { PerformanceTarget::GrainShape, "grain_shape", "Grain Shape", "G.Shape", true, true, true, false },
    { PerformanceTarget::FilterMorph, "filter_morph", "Filter Morph", "F.Morph", true, true, true, false },
    { PerformanceTarget::FilterEnable, "filter_enable", "Filter Enable", "F.En", true, false, false, false },
    { PerformanceTarget::SliceLength, "slice_length", "Slice Length", "Slice", true, false, false, false },
    { PerformanceTarget::Scratch, "scratch", "Scratch", "Scratch", true, false, false, false }
}};

inline constexpr std::array<PerformanceTarget, 24> kMacroPerformanceTargetOrder{{
    PerformanceTarget::None,
    PerformanceTarget::Cutoff,
    PerformanceTarget::Resonance,
    PerformanceTarget::FilterMorph,
    PerformanceTarget::Pitch,
    PerformanceTarget::Volume,
    PerformanceTarget::Pan,
    PerformanceTarget::FilterEnable,
    PerformanceTarget::Speed,
    PerformanceTarget::SliceLength,
    PerformanceTarget::Scratch,
    PerformanceTarget::GrainSize,
    PerformanceTarget::GrainDensity,
    PerformanceTarget::GrainPitch,
    PerformanceTarget::GrainPitchJitter,
    PerformanceTarget::GrainSpread,
    PerformanceTarget::GrainJitter,
    PerformanceTarget::GrainPositionJitter,
    PerformanceTarget::GrainRandom,
    PerformanceTarget::GrainArp,
    PerformanceTarget::GrainCloud,
    PerformanceTarget::GrainEmitter,
    PerformanceTarget::GrainEnvelope,
    PerformanceTarget::GrainShape
}};

inline constexpr std::array<PerformanceTarget, 22> kModPerformanceTargetOrder{{
    PerformanceTarget::None,
    PerformanceTarget::Volume,
    PerformanceTarget::Pan,
    PerformanceTarget::Pitch,
    PerformanceTarget::Speed,
    PerformanceTarget::Cutoff,
    PerformanceTarget::Resonance,
    PerformanceTarget::GrainSize,
    PerformanceTarget::GrainDensity,
    PerformanceTarget::GrainPitch,
    PerformanceTarget::GrainPitchJitter,
    PerformanceTarget::GrainSpread,
    PerformanceTarget::GrainJitter,
    PerformanceTarget::GrainRandom,
    PerformanceTarget::GrainArp,
    PerformanceTarget::GrainCloud,
    PerformanceTarget::GrainEmitter,
    PerformanceTarget::GrainEnvelope,
    PerformanceTarget::GrainPositionJitter,
    PerformanceTarget::GrainShape,
    PerformanceTarget::FilterMorph,
    PerformanceTarget::Retrigger
}};

inline constexpr int performanceTargetCount()
{
    return static_cast<int>(PerformanceTarget::Scratch) + 1;
}

inline constexpr bool isKnownPerformanceTargetValue(int rawValue)
{
    return rawValue >= 0 && rawValue < performanceTargetCount();
}

inline constexpr PerformanceTarget performanceTargetFromRaw(int rawValue)
{
    return isKnownPerformanceTargetValue(rawValue)
        ? static_cast<PerformanceTarget>(rawValue)
        : PerformanceTarget::None;
}

inline const PerformanceTargetInfo& performanceTargetInfo(PerformanceTarget target)
{
    const int rawValue = static_cast<int>(target);
    if (!isKnownPerformanceTargetValue(rawValue))
        return kPerformanceTargetInfos[0];

    return kPerformanceTargetInfos[static_cast<std::size_t>(rawValue)];
}

inline bool performanceTargetAllowsMacro(PerformanceTarget target)
{
    return performanceTargetInfo(target).macroAllowed;
}

inline bool performanceTargetAllowsModLane(PerformanceTarget target)
{
    return performanceTargetInfo(target).modLaneAllowed;
}

inline bool performanceTargetSupportsBipolar(PerformanceTarget target)
{
    return performanceTargetInfo(target).supportsBipolar;
}

inline bool performanceTargetSupportsPitchScaleQuantize(PerformanceTarget target)
{
    return performanceTargetInfo(target).supportsPitchScaleQuantize;
}

inline bool performanceTargetAutoDefaultBipolar(PerformanceTarget target)
{
    switch (target)
    {
        case PerformanceTarget::Pan:
        case PerformanceTarget::Pitch:
        case PerformanceTarget::GrainPitch:
        case PerformanceTarget::GrainSize:
        case PerformanceTarget::GrainShape:
        case PerformanceTarget::FilterMorph:
            return true;
        case PerformanceTarget::None:
        case PerformanceTarget::Volume:
        case PerformanceTarget::Speed:
        case PerformanceTarget::Cutoff:
        case PerformanceTarget::Resonance:
        case PerformanceTarget::GrainDensity:
        case PerformanceTarget::GrainPitchJitter:
        case PerformanceTarget::GrainSpread:
        case PerformanceTarget::GrainJitter:
        case PerformanceTarget::GrainRandom:
        case PerformanceTarget::GrainArp:
        case PerformanceTarget::GrainCloud:
        case PerformanceTarget::GrainEmitter:
        case PerformanceTarget::GrainEnvelope:
        case PerformanceTarget::Retrigger:
        case PerformanceTarget::GrainPositionJitter:
        case PerformanceTarget::FilterEnable:
        case PerformanceTarget::SliceLength:
        case PerformanceTarget::Scratch:
        default:
            return false;
    }
}

inline PerformanceTarget sanitizeMacroPerformanceTarget(PerformanceTarget target)
{
    const auto sanitized = performanceTargetFromRaw(static_cast<int>(target));
    return performanceTargetAllowsMacro(sanitized) ? sanitized : PerformanceTarget::None;
}

inline PerformanceTarget sanitizeModPerformanceTarget(PerformanceTarget target)
{
    const auto sanitized = performanceTargetFromRaw(static_cast<int>(target));
    return performanceTargetAllowsModLane(sanitized) ? sanitized : PerformanceTarget::None;
}

inline juce::String performanceTargetDisplayName(PerformanceTarget target, bool compact = false)
{
    const auto& info = performanceTargetInfo(target);
    return compact ? juce::String(info.compactDisplayName) : juce::String(info.displayName);
}

inline juce::String performanceTargetKey(PerformanceTarget target)
{
    return juce::String(performanceTargetInfo(target).key);
}

inline bool tryParsePerformanceTargetKey(const juce::String& key, PerformanceTarget& outTarget)
{
    const auto normalizedKey = key.trim().toLowerCase();
    if (normalizedKey.isEmpty())
        return false;

    for (const auto& info : kPerformanceTargetInfos)
    {
        if (normalizedKey == info.key)
        {
            outTarget = info.target;
            return true;
        }
    }

    return false;
}

inline int performanceTargetToComboId(PerformanceTarget target)
{
    return static_cast<int>(performanceTargetFromRaw(static_cast<int>(target))) + 1;
}

inline PerformanceTarget performanceTargetFromComboId(int comboId)
{
    return performanceTargetFromRaw(comboId - 1);
}

inline PerformanceTarget performanceTargetFromLegacyMacroRaw(int rawValue)
{
    switch (rawValue)
    {
        case 1: return PerformanceTarget::Cutoff;
        case 2: return PerformanceTarget::Resonance;
        case 3: return PerformanceTarget::FilterMorph;
        case 4: return PerformanceTarget::Pitch;
        case 5: return PerformanceTarget::Volume;
        case 6: return PerformanceTarget::Pan;
        case 7: return PerformanceTarget::FilterEnable;
        case 8: return PerformanceTarget::Speed;
        case 9: return PerformanceTarget::SliceLength;
        case 10: return PerformanceTarget::Scratch;
        case 11: return PerformanceTarget::GrainSize;
        case 12: return PerformanceTarget::GrainDensity;
        case 13: return PerformanceTarget::GrainPitch;
        case 14: return PerformanceTarget::GrainPitchJitter;
        case 15: return PerformanceTarget::GrainSpread;
        case 16: return PerformanceTarget::GrainJitter;
        case 17: return PerformanceTarget::GrainPositionJitter;
        case 18: return PerformanceTarget::GrainRandom;
        case 19: return PerformanceTarget::GrainArp;
        case 20: return PerformanceTarget::GrainCloud;
        case 21: return PerformanceTarget::GrainEmitter;
        case 22: return PerformanceTarget::GrainEnvelope;
        case 23: return PerformanceTarget::GrainShape;
        case 0:
        default:
            return PerformanceTarget::None;
    }
}

inline int legacyMacroRawFromPerformanceTarget(PerformanceTarget target)
{
    switch (sanitizeMacroPerformanceTarget(target))
    {
        case PerformanceTarget::Cutoff: return 1;
        case PerformanceTarget::Resonance: return 2;
        case PerformanceTarget::FilterMorph: return 3;
        case PerformanceTarget::Pitch: return 4;
        case PerformanceTarget::Volume: return 5;
        case PerformanceTarget::Pan: return 6;
        case PerformanceTarget::FilterEnable: return 7;
        case PerformanceTarget::Speed: return 8;
        case PerformanceTarget::SliceLength: return 9;
        case PerformanceTarget::Scratch: return 10;
        case PerformanceTarget::GrainSize: return 11;
        case PerformanceTarget::GrainDensity: return 12;
        case PerformanceTarget::GrainPitch: return 13;
        case PerformanceTarget::GrainPitchJitter: return 14;
        case PerformanceTarget::GrainSpread: return 15;
        case PerformanceTarget::GrainJitter: return 16;
        case PerformanceTarget::GrainPositionJitter: return 17;
        case PerformanceTarget::GrainRandom: return 18;
        case PerformanceTarget::GrainArp: return 19;
        case PerformanceTarget::GrainCloud: return 20;
        case PerformanceTarget::GrainEmitter: return 21;
        case PerformanceTarget::GrainEnvelope: return 22;
        case PerformanceTarget::GrainShape: return 23;
        case PerformanceTarget::None:
        case PerformanceTarget::Retrigger:
        default:
            return 0;
    }
}
