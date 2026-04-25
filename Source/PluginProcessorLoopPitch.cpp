/*
  ==============================================================================

    PluginProcessorLoopPitch.cpp
    Loop-pitch state/runtime implementation split from PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"

#include <cmath>

namespace
{
juce::String compactLoopPitchAnalysisStatus(juce::String statusText)
{
    const auto text = statusText.trim();
    if (text.isEmpty())
        return {};
    if (text.startsWithIgnoreCase("Loading "))
        return "Loading";
    if (text.containsIgnoreCase("Decoding"))
        return "Decoding";
    if (text.containsIgnoreCase("Waveform"))
        return "Waveform";
    if (text.containsIgnoreCase("Analyzing transients"))
        return "Transients";
    if (text.containsIgnoreCase("Preparing Essentia"))
        return "Preparing";
    if (text.containsIgnoreCase("Essentia"))
        return "Essentia";
    if (text.containsIgnoreCase("Snapping"))
        return "Snapping";
    if (text.containsIgnoreCase("Finalizing"))
        return "Finalizing";
    if (text.containsIgnoreCase("fallback"))
        return "Fallback";
    return text.upToFirstOccurrenceOf("...", false, false);
}

MlrVSTAudioProcessor::LoopPitchRole sanitizeLoopPitchRole(int roleValue)
{
    switch (roleValue)
    {
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::Master):
            return MlrVSTAudioProcessor::LoopPitchRole::Master;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::Sync):
            return MlrVSTAudioProcessor::LoopPitchRole::Sync;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::None):
        default:
            return MlrVSTAudioProcessor::LoopPitchRole::None;
    }
}

MlrVSTAudioProcessor::LoopPitchSyncTiming sanitizeLoopPitchSyncTiming(int timingValue)
{
    switch (timingValue)
    {
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextTrigger):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextTrigger;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextLoop):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextLoop;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::Immediate):
        default:
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::Immediate;
    }
}

float computeNearestPitchClassCorrectionSemitones(float sourceMidi, int targetRootMidi) noexcept
{
    if (!std::isfinite(sourceMidi))
        return 0.0f;

    const float sourcePitchClass = std::fmod(sourceMidi, 12.0f);
    const float wrappedSourcePitchClass = sourcePitchClass < 0.0f ? sourcePitchClass + 12.0f : sourcePitchClass;
    const int wrappedTargetPitchClass = ((targetRootMidi % 12) + 12) % 12;
    float delta = static_cast<float>(wrappedTargetPitchClass) - wrappedSourcePitchClass;
    while (delta > 6.0f)
        delta -= 12.0f;
    while (delta < -6.0f)
        delta += 12.0f;
    return delta;
}

float semitonesFromRatio(float ratio)
{
    const float safeRatio = juce::jlimit(0.03125f, 8.0f, ratio);
    return static_cast<float>(12.0 * std::log2(static_cast<double>(safeRatio)));
}

float computeFlipTempoMatchRatio(double hostTempo, double sourceTempo)
{
    if (!std::isfinite(hostTempo) || !std::isfinite(sourceTempo) || hostTempo <= 0.0 || sourceTempo <= 0.0)
        return 1.0f;

    return juce::jlimit(0.25f, 4.0f, static_cast<float>(hostTempo / sourceTempo));
}
} // namespace

MlrVSTAudioProcessor::PreparedSceneLoopPitchState
MlrVSTAudioProcessor::capturePreparedSceneLoopPitchState(int stripIndex) const
{
    PreparedSceneLoopPitchState state;
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return state;

    const auto idx = static_cast<size_t>(stripIndex);
    state.valid = true;
    state.role = sanitizeLoopPitchRole(loopPitchRoles[idx].load(std::memory_order_acquire));
    state.syncTiming = sanitizeLoopPitchSyncTiming(loopPitchSyncTimings[idx].load(std::memory_order_acquire));
    state.assignedMidi = juce::jlimit(-1, 127, loopPitchAssignedMidi[idx].load(std::memory_order_acquire));
    state.assignedManual = loopPitchAssignedManual[idx].load(std::memory_order_acquire) != 0;
    state.detectedMidi = juce::jlimit(-1, 127, loopPitchDetectedMidi[idx].load(std::memory_order_acquire));
    state.detectedHz = juce::jlimit(0.0f, 24000.0f, loopPitchDetectedHz[idx].load(std::memory_order_acquire));
    state.detectedPitchConfidence = juce::jlimit(
        0.0f,
        1.0f,
        loopPitchDetectedPitchConfidence[idx].load(std::memory_order_acquire));
    state.detectedScaleIndex = juce::jlimit(
        -1,
        static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
        loopPitchDetectedScaleIndices[idx].load(std::memory_order_acquire));
    state.detectedScaleConfidence = juce::jlimit(
        0.0f,
        1.0f,
        loopPitchDetectedScaleConfidence[idx].load(std::memory_order_acquire));
    state.essentiaUsed = loopPitchEssentiaUsed[idx].load(std::memory_order_acquire) != 0;
    return state;
}

void MlrVSTAudioProcessor::applyPreparedSceneLoopPitchState(int stripIndex,
                                                            const PreparedSceneLoopPitchState& state)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    loopPitchRoles[idx].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    loopPitchSyncTimings[idx].store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    loopPitchAssignedMidi[idx].store(-1, std::memory_order_release);
    loopPitchAssignedManual[idx].store(0, std::memory_order_release);
    loopPitchDetectedMidi[idx].store(-1, std::memory_order_release);
    loopPitchDetectedHz[idx].store(0.0f, std::memory_order_release);
    loopPitchDetectedPitchConfidence[idx].store(0.0f, std::memory_order_release);
    loopPitchDetectedScaleIndices[idx].store(-1, std::memory_order_release);
    loopPitchDetectedScaleConfidence[idx].store(0.0f, std::memory_order_release);
    loopPitchEssentiaUsed[idx].store(0, std::memory_order_release);
    loopPitchPendingRetune[idx].store(0, std::memory_order_release);

    if (!state.valid)
        return;

    loopPitchRoles[idx].store(static_cast<int>(sanitizeLoopPitchRole(static_cast<int>(state.role))),
                              std::memory_order_release);
    loopPitchSyncTimings[idx].store(
        static_cast<int>(sanitizeLoopPitchSyncTiming(static_cast<int>(state.syncTiming))),
        std::memory_order_release);
    loopPitchAssignedMidi[idx].store(juce::jlimit(-1, 127, state.assignedMidi), std::memory_order_release);
    loopPitchAssignedManual[idx].store(state.assignedManual ? 1 : 0, std::memory_order_release);
    loopPitchDetectedMidi[idx].store(juce::jlimit(-1, 127, state.detectedMidi), std::memory_order_release);
    loopPitchDetectedHz[idx].store(juce::jlimit(0.0f, 24000.0f, state.detectedHz), std::memory_order_release);
    loopPitchDetectedPitchConfidence[idx].store(
        juce::jlimit(0.0f, 1.0f, state.detectedPitchConfidence),
        std::memory_order_release);
    loopPitchDetectedScaleIndices[idx].store(
        juce::jlimit(-1,
                     static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                     state.detectedScaleIndex),
        std::memory_order_release);
    loopPitchDetectedScaleConfidence[idx].store(
        juce::jlimit(0.0f, 1.0f, state.detectedScaleConfidence),
        std::memory_order_release);
    loopPitchEssentiaUsed[idx].store(state.essentiaUsed ? 1 : 0, std::memory_order_release);
    requestLoopPitchRoleStateUpdate(stripIndex);
}

MlrVSTAudioProcessor::LoopPitchRole MlrVSTAudioProcessor::getLoopPitchRole(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return LoopPitchRole::None;
    return sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
}

int MlrVSTAudioProcessor::getLoopPitchMasterStripIndex() const
{
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire))
            == LoopPitchRole::Master)
        {
            return i;
        }
    }

    return -1;
}

MlrVSTAudioProcessor::LoopPitchSyncTiming MlrVSTAudioProcessor::getLoopPitchSyncTiming(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return LoopPitchSyncTiming::Immediate;
    return sanitizeLoopPitchSyncTiming(loopPitchSyncTimings[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
}

void MlrVSTAudioProcessor::setLoopPitchRole(int stripIndex, LoopPitchRole role)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto previousRole = getLoopPitchRole(stripIndex);
    const auto clampedRole = sanitizeLoopPitchRole(static_cast<int>(role));
    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    if (clampedRole == LoopPitchRole::Master)
    {
        for (int i = 0; i < MaxStrips; ++i)
        {
            if (i == stripIndex)
                continue;
            if (getLoopPitchRole(i) == LoopPitchRole::Master)
            {
                loopPitchRoles[static_cast<size_t>(i)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
                loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
            }
        }
    }

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(static_cast<int>(clampedRole), std::memory_order_release);

    if (previousRole == LoopPitchRole::Sync && clampedRole != LoopPitchRole::Sync)
        applyStoredPitchControlToStrip(stripIndex);

    if (clampedRole == LoopPitchRole::Master)
    {
        if (getLoopStripAssignedPitchMidi(stripIndex) >= 0 || getLoopStripDetectedPitchMidi(stripIndex) >= 0)
            updateGlobalRootFromLoopPitchMaster(stripIndex, true);
        requestLoopStripPitchMaster(stripIndex);
    }
    else if (clampedRole == LoopPitchRole::Sync)
    {
        if (getLoopStripAssignedPitchMidi(stripIndex) >= 0)
            requestLoopPitchRoleStateUpdate(stripIndex);
        else
            requestLoopStripPitchSync(stripIndex);
    }

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::setLoopPitchSyncTiming(int stripIndex, LoopPitchSyncTiming timing)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchSyncTiming(static_cast<int>(timing))),
        std::memory_order_release);

    if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
        requestLoopPitchRoleStateUpdate(stripIndex);

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

int MlrVSTAudioProcessor::getLoopStripAssignedPitchMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;
    return loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

float MlrVSTAudioProcessor::getLoopStripPitchSyncCorrectionSemitones(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
        return 0.0f;

    return computeNearestPitchClassCorrectionSemitones(sourceMidi, getGlobalRootNoteMidi());
}

void MlrVSTAudioProcessor::setLoopStripAssignedPitchMidi(int stripIndex, int midiNote, bool manualOverride)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int clampedMidi = juce::jlimit(-1, 127, midiNote);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(clampedMidi, std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(manualOverride ? 1 : 0, std::memory_order_release);

    const auto role = getLoopPitchRole(stripIndex);
    if (role == LoopPitchRole::Master && clampedMidi >= 0)
        updateGlobalRootFromLoopPitchMaster(stripIndex, true);
    else if (role == LoopPitchRole::Sync && clampedMidi >= 0)
        requestLoopPitchRoleStateUpdate(stripIndex);

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::updateGlobalRootFromLoopPitchMaster(int stripIndex, bool markPersistent)
{
    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Master)
        return;

    const int effectiveMidi = getEffectiveLoopPitchMasterRootMidi(stripIndex);
    if (effectiveMidi >= 0)
        setGlobalRootNoteMidi(effectiveMidi, markPersistent);
}

int MlrVSTAudioProcessor::getGlobalRootNotePitchClass() const
{
    const int midi = getGlobalRootNoteMidi();
    const int pitchClass = midi % 12;
    return pitchClass < 0 ? pitchClass + 12 : pitchClass;
}

void MlrVSTAudioProcessor::setGlobalRootNoteMidi(int midiNote, bool markPersistent)
{
    const int clamped = juce::jlimit(0, 127, midiNote);
    const int previous = globalRootNoteMidi.exchange(clamped, std::memory_order_acq_rel);
    if (markPersistent)
        markPersistentGlobalUserChange();
    if (previous != clamped)
    {
        reapplyGlobalPitchQuantizationToAllStrips();
        applyLoopPitchSyncToAllStrips();
    }
}

void MlrVSTAudioProcessor::setGlobalRootNotePitchClass(int pitchClass)
{
    const int masterStripIndex = getLoopPitchMasterStripIndex();
    if (masterStripIndex >= 0)
    {
        updateGlobalRootFromLoopPitchMaster(masterStripIndex, true);
        return;
    }

    const int clampedPitchClass = ((pitchClass % 12) + 12) % 12;
    const int currentMidi = getGlobalRootNoteMidi();
    const int octave = juce::jlimit(0, 10, currentMidi / 12);
    int updatedMidi = (octave * 12) + clampedPitchClass;
    if (updatedMidi > 127)
        updatedMidi -= 12;
    setGlobalRootNoteMidi(updatedMidi, true);
}

ModernAudioEngine::PitchScale MlrVSTAudioProcessor::getGlobalPitchScale() const
{
    return static_cast<ModernAudioEngine::PitchScale>(juce::jlimit(
        0,
        static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
        globalPitchScale.load(std::memory_order_acquire)));
}

void MlrVSTAudioProcessor::setGlobalPitchScale(ModernAudioEngine::PitchScale scale)
{
    const int clamped = juce::jlimit(0,
                                     static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                                     static_cast<int>(scale));
    globalPitchScale.store(clamped, std::memory_order_release);
    markPersistentGlobalUserChange();
    reapplyGlobalPitchQuantizationToAllStrips();
    applyLoopPitchSyncToAllStrips();
}

float MlrVSTAudioProcessor::quantizePitchSemitonesToGlobalScale(float semitones) const
{
    return ModernAudioEngine::quantizePitchSemitonesToScale(semitones,
                                                            getGlobalRootNoteMidi(),
                                                            getGlobalPitchScale());
}

void MlrVSTAudioProcessor::reapplyGlobalPitchQuantizationToAllStrips()
{
    if (audioEngine == nullptr)
        return;

    const int globalRootMidi = getGlobalRootNoteMidi();
    const auto globalScale = getGlobalPitchScale();

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        strip->setGlobalPitchContext(globalRootMidi, static_cast<int>(globalScale));

        float semitones = strip->getPitchShift();
        if (auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
            semitones = rawParam->load(std::memory_order_acquire);

        const float quantizedSemitones = quantizePitchSemitonesForStripControl(stripIndex, semitones);
        if (auto* parameter = parameters.getParameter("stripPitch" + juce::String(stripIndex)))
        {
            const float normalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(quantizedSemitones));
            const float currentNormalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(semitones));
            if (std::abs(currentNormalized - normalized) > 1.0e-5f)
                parameter->setValueNotifyingHost(normalized);
        }

        if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
        {
            applyPitchControlToStrip(stripIndex, *strip, getPitchSemitonesForDisplay(*strip));
            continue;
        }

        applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);
    }
}

void MlrVSTAudioProcessor::queueLoopPitchAnalysisResult(LoopPitchAnalysisResult result)
{
    const juce::ScopedLock lock(loopPitchAnalysisResultLock);
    loopPitchAnalysisResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::updateLoopPitchAnalysisProgress(int stripIndex,
                                                           int requestId,
                                                           float progress,
                                                           const juce::String& statusText)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (requestId != loopPitchAnalysisRequestIds[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire))
        return;

    loopPitchAnalysisProgressPermille[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0, 1000, static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, progress) * 1000.0f))),
        std::memory_order_release);

    const juce::ScopedLock lock(loopPitchAnalysisStatusLock);
    loopPitchAnalysisStatusTexts[static_cast<size_t>(stripIndex)] = compactLoopPitchAnalysisStatus(statusText);
}

void MlrVSTAudioProcessor::resetLoopPitchAnalysisProgress(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchAnalysisProgressPermille[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    const juce::ScopedLock lock(loopPitchAnalysisStatusLock);
    loopPitchAnalysisStatusTexts[static_cast<size_t>(stripIndex)].clear();
}

void MlrVSTAudioProcessor::applyCompletedLoopPitchAnalyses()
{
    std::vector<LoopPitchAnalysisResult> results;
    {
        const juce::ScopedLock lock(loopPitchAnalysisResultLock);
        if (loopPitchAnalysisResults.empty())
            return;
        results.swap(loopPitchAnalysisResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips)
            continue;

        auto& inFlight = loopPitchAnalysisInFlight[static_cast<size_t>(result.stripIndex)];
        auto& requestCounter = loopPitchAnalysisRequestIds[static_cast<size_t>(result.stripIndex)];
        if (result.requestId != requestCounter.load(std::memory_order_acquire))
            continue;

        inFlight.store(0, std::memory_order_release);
        resetLoopPitchAnalysisProgress(result.stripIndex);
        loopPitchDetectedMidi[static_cast<size_t>(result.stripIndex)].store(result.detectedMidi, std::memory_order_release);
        loopPitchDetectedHz[static_cast<size_t>(result.stripIndex)].store(static_cast<float>(result.detectedHz), std::memory_order_release);
        loopPitchDetectedPitchConfidence[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, result.detectedPitchConfidence),
            std::memory_order_release);
        loopPitchDetectedScaleIndices[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(-1,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         result.detectedScaleIndex),
            std::memory_order_release);
        loopPitchDetectedScaleConfidence[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, result.detectedScaleConfidence),
            std::memory_order_release);
        loopPitchEssentiaUsed[static_cast<size_t>(result.stripIndex)].store(result.essentiaUsed ? 1 : 0, std::memory_order_release);
        const auto role = getLoopPitchRole(result.stripIndex);

        if (result.setAsRoot && role == LoopPitchRole::Master && result.detectedScaleIndex >= 0)
            setGlobalPitchScale(static_cast<ModernAudioEngine::PitchScale>(result.detectedScaleIndex));

        if (!result.success || result.detectedMidi < 0)
            continue;

        loopPitchAssignedMidi[static_cast<size_t>(result.stripIndex)].store(result.detectedMidi, std::memory_order_release);
        loopPitchAssignedManual[static_cast<size_t>(result.stripIndex)].store(0, std::memory_order_release);

        if (result.setAsRoot && role == LoopPitchRole::Master)
        {
            updateGlobalRootFromLoopPitchMaster(result.stripIndex, true);
            continue;
        }

        if (!result.setAsRoot && role == LoopPitchRole::Sync)
            requestLoopPitchRoleStateUpdate(result.stripIndex);
    }
}

void MlrVSTAudioProcessor::applyLoopStripPitchSemitones(int stripIndex, float semitones)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    const float storedSemitones = quantizePitchSemitonesForStripControl(stripIndex,
                                                                        getStoredStripPitchSemitones(stripIndex));
    const float clamped = quantizePitchSemitonesForStripControl(stripIndex,
                                                                juce::jlimit(-24.0f,
                                                                             24.0f,
                                                                             storedSemitones + semitones));

    if (auto* strip = audioEngine->getStrip(stripIndex))
        applyPitchControlToStrip(stripIndex, *strip, clamped);
}

void MlrVSTAudioProcessor::normalizeLoopPitchMasterRoles()
{
    int keeperStrip = -1;
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire))
            != LoopPitchRole::Master)
            continue;

        if (keeperStrip < 0)
        {
            keeperStrip = i;
            continue;
        }

        loopPitchRoles[static_cast<size_t>(i)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
        loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
    }
}

int MlrVSTAudioProcessor::getEffectiveLoopPitchMasterRootMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;

    float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
        sourceMidi = static_cast<float>(getGlobalRootNoteMidi());

    float currentPitchSemitones = 0.0f;
    if (audioEngine != nullptr)
    {
        if (auto* strip = audioEngine->getStrip(stripIndex))
            currentPitchSemitones = getPitchSemitonesForDisplay(*strip);
    }

    return juce::jlimit(0,
                        127,
                        juce::roundToInt(sourceMidi + currentPitchSemitones));
}

int MlrVSTAudioProcessor::getPitchQuantizeReferenceRootMidiForStrip(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return getGlobalRootNoteMidi();

    if (getLoopPitchRole(stripIndex) == LoopPitchRole::Master)
    {
        const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
        if (sourceMidi >= 0.0f)
            return juce::jlimit(0, 127, juce::roundToInt(sourceMidi));
    }

    return getGlobalRootNoteMidi();
}

float MlrVSTAudioProcessor::getLoopPitchTempoMatchOffsetSemitones(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return 0.0f;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return 0.0f;

    if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
    {
        if (!resolveFlipTempoMatch().usesRepitch())
            return 0.0f;

        auto* engine = sampleModeEngines[static_cast<size_t>(stripIndex)].get();
        if (engine == nullptr)
            return 0.0f;

        const float tempoMatchRatio = computeFlipTempoMatchRatio(audioEngine->getCurrentTempo(),
                                                                 engine->getAnalyzedTempoBpm());
        if (std::abs(tempoMatchRatio - 1.0f) <= 1.0e-4f)
            return 0.0f;

        return semitonesFromRatio(tempoMatchRatio);
    }

    if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
        || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
        || resolveLoopTempoMatchBackendForStrip(stripIndex) != TimeStretchBackend::Resample)
    {
        return 0.0f;
    }

    const auto* stripBuffer = strip->getAudioBuffer();
    const double hostTempo = audioEngine->getCurrentTempo();
    if (stripBuffer == nullptr
        || stripBuffer->getNumSamples() <= 0
        || !(currentSampleRate > 0.0)
        || !(hostTempo > 0.0))
    {
        return 0.0f;
    }

    float beatsForLoop = strip->getBeatsPerLoop();
    if (!(beatsForLoop > 0.0f))
        beatsForLoop = 4.0f;

    const double targetLoopLengthInSamples = static_cast<double>(beatsForLoop)
        * (60.0 / hostTempo)
        * currentSampleRate;
    if (!(targetLoopLengthInSamples > 0.0))
        return 0.0f;

    const double autoWarpSpeed = static_cast<double>(stripBuffer->getNumSamples()) / targetLoopLengthInSamples;
    if (!std::isfinite(autoWarpSpeed) || std::abs(autoWarpSpeed - 1.0) <= 1.0e-4)
        return 0.0f;

    return semitonesFromRatio(static_cast<float>(autoWarpSpeed));
}

float MlrVSTAudioProcessor::getEffectiveLoopPitchSourceMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1.0f;

    int sourceMidi = getLoopStripAssignedPitchMidi(stripIndex);
    if (sourceMidi < 0)
        sourceMidi = getLoopStripDetectedPitchMidi(stripIndex);
    if (sourceMidi < 0)
        return -1.0f;

    return static_cast<float>(sourceMidi) + getLoopPitchTempoMatchOffsetSemitones(stripIndex);
}

void MlrVSTAudioProcessor::applyLoopPitchRoleStateToStrip(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
        return;

    const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
    {
        loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
        applyStoredPitchControlToStrip(stripIndex);
        return;
    }

    const float deltaSemitones = computeNearestPitchClassCorrectionSemitones(
        sourceMidi,
        getGlobalRootNoteMidi());

    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    applyLoopStripPitchSemitones(stripIndex, deltaSemitones);
}

void MlrVSTAudioProcessor::requestLoopPitchRoleStateUpdate(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto timing = getLoopPitchSyncTiming(stripIndex);
    const bool immediate = timing == LoopPitchSyncTiming::Immediate
        || !strip->isPlaying()
        || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;

    if (immediate)
    {
        applyLoopPitchRoleStateToStrip(stripIndex);
        return;
    }

    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(1, std::memory_order_release);
}

bool MlrVSTAudioProcessor::applyPendingLoopPitchRetuneOnTrigger(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
    {
        loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
        return false;
    }

    if (loopPitchPendingRetune[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) == 0)
        return false;

    if (getLoopPitchSyncTiming(stripIndex) != LoopPitchSyncTiming::NextTrigger)
        return false;

    applyLoopPitchRoleStateToStrip(stripIndex);
    return true;
}

void MlrVSTAudioProcessor::applyPendingLoopPitchRetunes()
{
    if (audioEngine == nullptr)
        return;

    const double timelineBeat = audioEngine->getTimelineBeat();
    const int currentHostBar = std::isfinite(timelineBeat)
        ? juce::jmax(0, static_cast<int>(std::floor(timelineBeat / 4.0)))
        : -1;

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        const int currentColumn = (strip != nullptr && strip->isPlaying()) ? strip->getCurrentColumn() : -1;

        if (loopPitchPendingRetune[static_cast<size_t>(i)].load(std::memory_order_acquire) != 0
            && strip != nullptr
            && strip->isPlaying())
        {
            if (getLoopPitchRole(i) != LoopPitchRole::Sync)
            {
                loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
                loopPitchLastObservedColumns[static_cast<size_t>(i)] = currentColumn;
                continue;
            }

            switch (getLoopPitchSyncTiming(i))
            {
                case LoopPitchSyncTiming::NextLoop:
                    if (loopPitchLastObservedColumns[static_cast<size_t>(i)] >= 0
                        && currentColumn >= 0
                        && currentColumn < loopPitchLastObservedColumns[static_cast<size_t>(i)])
                        applyLoopPitchRoleStateToStrip(i);
                    break;
                case LoopPitchSyncTiming::NextBar:
                    if (currentHostBar >= 0
                        && loopPitchLastObservedHostBar >= 0
                        && currentHostBar != loopPitchLastObservedHostBar)
                        applyLoopPitchRoleStateToStrip(i);
                    break;
                case LoopPitchSyncTiming::Immediate:
                case LoopPitchSyncTiming::NextTrigger:
                default:
                    break;
            }
        }

        loopPitchLastObservedColumns[static_cast<size_t>(i)] = currentColumn;
    }

    if (currentHostBar >= 0)
        loopPitchLastObservedHostBar = currentHostBar;
}

void MlrVSTAudioProcessor::applyLoopPitchSyncToAllStrips()
{
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (getLoopPitchRole(i) == LoopPitchRole::Sync)
            requestLoopPitchRoleStateUpdate(i);
    }
}

void MlrVSTAudioProcessor::resolveLoopPitchRecallStateImmediately()
{
    if (audioEngine == nullptr)
        return;

    normalizeLoopPitchMasterRoles();

    const int masterStripIndex = getLoopPitchMasterStripIndex();
    if (masterStripIndex >= 0)
    {
        const int effectiveMidi = getEffectiveLoopPitchMasterRootMidi(masterStripIndex);
        if (effectiveMidi >= 0)
            globalRootNoteMidi.store(juce::jlimit(0, 127, effectiveMidi), std::memory_order_release);
    }

    const int rootMidi = getGlobalRootNoteMidi();
    const auto scale = getGlobalPitchScale();
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        if (auto* strip = audioEngine->getStrip(stripIndex))
            strip->setGlobalPitchContext(rootMidi, static_cast<int>(scale));

        if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
            applyLoopPitchRoleStateToStrip(stripIndex);
        else
            loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    }

    loopPitchLastObservedColumns.fill(-1);
    loopPitchLastObservedHostBar = -1;
}

void MlrVSTAudioProcessor::appendLoopPitchStateToState(juce::ValueTree& state) const
{
    auto loopPitchState = state.getOrCreateChildWithName("LoopPitchState", nullptr);
    while (loopPitchState.getNumChildren() > 0)
        loopPitchState.removeChild(0, nullptr);

    for (int i = 0; i < MaxStrips; ++i)
    {
        juce::ValueTree stripState("Strip");
        stripState.setProperty("index", i, nullptr);
        stripState.setProperty("role", loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("syncTiming", loopPitchSyncTimings[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("assignedMidi", loopPitchAssignedMidi[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("assignedManual", loopPitchAssignedManual[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedMidi", loopPitchDetectedMidi[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedHz", loopPitchDetectedHz[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedPitchConfidence", loopPitchDetectedPitchConfidence[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedScaleIndex", loopPitchDetectedScaleIndices[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedScaleConfidence", loopPitchDetectedScaleConfidence[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("essentiaUsed", loopPitchEssentiaUsed[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        loopPitchState.addChild(stripState, -1, nullptr);
    }
}

void MlrVSTAudioProcessor::loadLoopPitchStateFromState(const juce::ValueTree& state)
{
    for (auto& role : loopPitchRoles)
        role.store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    for (auto& assignedMidi : loopPitchAssignedMidi)
        assignedMidi.store(-1, std::memory_order_release);
    for (auto& detectedMidi : loopPitchDetectedMidi)
        detectedMidi.store(-1, std::memory_order_release);
    for (auto& detectedHz : loopPitchDetectedHz)
        detectedHz.store(0.0f, std::memory_order_release);
    for (auto& detectedPitchConfidence : loopPitchDetectedPitchConfidence)
        detectedPitchConfidence.store(0.0f, std::memory_order_release);
    for (auto& detectedScale : loopPitchDetectedScaleIndices)
        detectedScale.store(-1, std::memory_order_release);
    for (auto& detectedScaleConfidence : loopPitchDetectedScaleConfidence)
        detectedScaleConfidence.store(0.0f, std::memory_order_release);
    for (auto& essentiaUsed : loopPitchEssentiaUsed)
        essentiaUsed.store(0, std::memory_order_release);
    for (auto& timing : loopPitchSyncTimings)
        timing.store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    for (auto& assignedManual : loopPitchAssignedManual)
        assignedManual.store(0, std::memory_order_release);
    for (auto& pendingRetune : loopPitchPendingRetune)
        pendingRetune.store(0, std::memory_order_release);

    auto loopPitchState = state.getChildWithName("LoopPitchState");
    if (!loopPitchState.isValid())
        return;

    for (auto stripState : loopPitchState)
    {
        if (!stripState.hasType("Strip"))
            continue;

        const int stripIndex = static_cast<int>(stripState.getProperty("index", -1));
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            continue;

        loopPitchRoles[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(sanitizeLoopPitchRole(static_cast<int>(stripState.getProperty("role", 0)))),
            std::memory_order_release);
        loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(sanitizeLoopPitchSyncTiming(static_cast<int>(stripState.getProperty("syncTiming", 0)))),
            std::memory_order_release);
        loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1, 127, static_cast<int>(stripState.getProperty("assignedMidi", -1))),
            std::memory_order_release);
        loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(stripState.getProperty("assignedManual", 0)) != 0 ? 1 : 0,
            std::memory_order_release);
        loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1, 127, static_cast<int>(stripState.getProperty("detectedMidi", -1))),
            std::memory_order_release);
        loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 24000.0f, static_cast<float>(stripState.getProperty("detectedHz", 0.0f))),
            std::memory_order_release);
        loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, static_cast<float>(stripState.getProperty("detectedPitchConfidence", 0.0f))),
            std::memory_order_release);
        loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         static_cast<int>(stripState.getProperty("detectedScaleIndex", -1))),
            std::memory_order_release);
        loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, static_cast<float>(stripState.getProperty("detectedScaleConfidence", 0.0f))),
            std::memory_order_release);
        loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(stripState.getProperty("essentiaUsed", 0)) != 0 ? 1 : 0,
            std::memory_order_release);
    }

    normalizeLoopPitchMasterRoles();
    applyLoopPitchSyncToAllStrips();
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createLoopPitchPresetStateXml(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    auto xml = std::make_unique<juce::XmlElement>("LoopPitchState");
    xml->setAttribute("role", loopPitchRoles[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("syncTiming", loopPitchSyncTimings[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("assignedMidi", loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("assignedManual", loopPitchAssignedManual[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedMidi", loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedHz", static_cast<double>(loopPitchDetectedHz[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("detectedPitchConfidence", static_cast<double>(loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("detectedScaleIndex", loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedScaleConfidence", static_cast<double>(loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("essentiaUsed", loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    return xml;
}

void MlrVSTAudioProcessor::applyLoopPitchPresetStateXml(int stripIndex, const juce::XmlElement* stateXml)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);

    if (stateXml == nullptr || !stateXml->hasTagName("LoopPitchState"))
        return;

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchRole(stateXml->getIntAttribute("role", 0))),
        std::memory_order_release);
    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchSyncTiming(stateXml->getIntAttribute("syncTiming", 0))),
        std::memory_order_release);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1, 127, stateXml->getIntAttribute("assignedMidi", -1)),
        std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(
        stateXml->getBoolAttribute("assignedManual", false) ? 1 : 0,
        std::memory_order_release);
    loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1, 127, stateXml->getIntAttribute("detectedMidi", -1)),
        std::memory_order_release);
    loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 24000.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedHz", 0.0))),
        std::memory_order_release);
    loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 1.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedPitchConfidence", 0.0))),
        std::memory_order_release);
    loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1,
                     static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                     stateXml->getIntAttribute("detectedScaleIndex", -1)),
        std::memory_order_release);
    loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 1.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedScaleConfidence", 0.0))),
        std::memory_order_release);
    loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(
        stateXml->getBoolAttribute("essentiaUsed", false) ? 1 : 0,
        std::memory_order_release);

    requestLoopPitchRoleStateUpdate(stripIndex);
}
