/*
  ==============================================================================

    SceneScheduler.cpp
    Scene timing, scheduling, and persistence helpers

  ==============================================================================
*/

#include "SceneScheduler.h"
#include "PluginProcessor.h"
#include "PresetStore.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using SceneLengthMode = MlrVSTAudioProcessor::SceneLengthMode;
using SceneRecallMode = MlrVSTAudioProcessor::SceneRecallMode;

SceneLengthMode sanitizeSceneLengthMode(int rawMode)
{
    return static_cast<SceneLengthMode>(juce::jlimit(
        0,
        static_cast<int>(SceneLengthMode::AnchorStrip),
        rawMode));
}

SceneRecallMode sanitizeSceneRecallMode(int rawMode)
{
    return static_cast<SceneRecallMode>(juce::jlimit(
        0,
        static_cast<int>(SceneRecallMode::Manual),
        rawMode));
}

struct ScopedSuspendProcessing
{
    explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
    ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
    MlrVSTAudioProcessor& processor;
};
} // namespace

int SceneScheduler::getSceneRepeatCount(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneRepeatCount, processor.sceneRepeatCounts[idx]);
}

void SceneScheduler::setSceneRepeatCount(MlrVSTAudioProcessor& processor, int sceneSlot, int repeats)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int clampedRepeats = juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneRepeatCount, repeats);
    if (processor.sceneRepeatCounts[idx] == clampedRepeats)
        return;

    processor.sceneRepeatCounts[idx] = clampedRepeats;
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneLengthModeIndex(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return static_cast<int>(sanitizeSceneLengthMode(processor.sceneLengthModes[idx]));
}

void SceneScheduler::setSceneLengthModeIndex(MlrVSTAudioProcessor& processor, int sceneSlot, int modeIndex)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const auto previousMode = sanitizeSceneLengthMode(processor.sceneLengthModes[idx]);
    const int clampedMode = static_cast<int>(sanitizeSceneLengthMode(modeIndex));
    if (processor.sceneLengthModes[idx] == clampedMode)
        return;

    processor.sceneLengthModes[idx] = clampedMode;
    const auto newMode = sanitizeSceneLengthMode(clampedMode);
    if (newMode == SceneLengthMode::ManualBars)
        setSceneRepeatCount(processor, sceneSlot, 1);
    else if (previousMode == SceneLengthMode::ManualBars)
        setSceneRepeatCount(processor, sceneSlot, getSceneManualBars(processor, sceneSlot));
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneRecallModeIndex(const MlrVSTAudioProcessor& processor)
{
    const int rawMode = processor.sceneRecallModeParam != nullptr
        ? static_cast<int>(processor.sceneRecallModeParam->load(std::memory_order_acquire))
        : static_cast<int>(SceneRecallMode::PatternEnd);
    return static_cast<int>(sanitizeSceneRecallMode(rawMode));
}

void SceneScheduler::setSceneRecallModeIndex(MlrVSTAudioProcessor& processor, int modeIndex)
{
    const int clampedMode = static_cast<int>(sanitizeSceneRecallMode(modeIndex));
    bool changed = false;
    if (auto* parameter = processor.parameters.getParameter("sceneRecallMode"))
    {
        const float currentNormalized = parameter->getValue();
        const float targetNormalized = parameter->convertTo0to1(static_cast<float>(clampedMode));
        if (std::abs(currentNormalized - targetNormalized) > 1.0e-6f)
        {
            parameter->setValueNotifyingHost(targetNormalized);
            changed = true;
        }
    }

    if (processor.pendingSceneRecall.active)
        processor.pendingSceneRecall.targetResolved = false;

    if (changed)
        processor.markPersistentGlobalUserChange();
}

int SceneScheduler::getSceneManualBars(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneManualBars, processor.sceneManualBars[idx]);
}

void SceneScheduler::setSceneManualBars(MlrVSTAudioProcessor& processor, int sceneSlot, int bars)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int clampedBars = juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneManualBars, bars);
    if (processor.sceneManualBars[idx] == clampedBars)
        return;

    processor.sceneManualBars[idx] = clampedBars;
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneAnchorStrip(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, processor.sceneAnchorStrips[idx]);
}

void SceneScheduler::setSceneAnchorStrip(MlrVSTAudioProcessor& processor, int sceneSlot, int stripIndex)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int clampedStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    if (processor.sceneAnchorStrips[idx] == clampedStrip)
        return;

    processor.sceneAnchorStrips[idx] = clampedStrip;
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneLengthCount(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    return getSceneLengthModeIndex(processor, sceneSlot) == static_cast<int>(SceneLengthMode::ManualBars)
        ? getSceneManualBars(processor, sceneSlot)
        : getSceneRepeatCount(processor, sceneSlot);
}

void SceneScheduler::setSceneLengthCount(MlrVSTAudioProcessor& processor, int sceneSlot, int count)
{
    if (getSceneLengthModeIndex(processor, sceneSlot) == static_cast<int>(SceneLengthMode::ManualBars))
    {
        setSceneManualBars(processor, sceneSlot, count);
        setSceneRepeatCount(processor, sceneSlot, 1);
        return;
    }

    setSceneRepeatCount(processor, sceneSlot, count);
}

double SceneScheduler::computeStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    if (processor.audioEngine == nullptr || stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return 0.0;

    auto* strip = processor.audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return 0.0;

    const auto playMode = strip->getPlayMode();
    const bool hasStripAudio = (playMode == EnhancedAudioStrip::PlayMode::Sample)
        ? processor.hasSampleModeAudio(stripIndex)
        : strip->hasAudio();

    if (playMode == EnhancedAudioStrip::PlayMode::Step)
    {
        const int totalSteps = strip->getStepPatternLengthSteps();
        const bool hasEnabledSteps = std::any_of(
            strip->stepPattern.begin(),
            strip->stepPattern.begin() + totalSteps,
            [](bool enabled) { return enabled; });
        if (!hasStripAudio && !hasEnabledSteps)
            return 0.0;

        return juce::jlimit(1.0, 256.0, static_cast<double>(strip->getStepPatternBars()) * 4.0);
    }

    if (!hasStripAudio)
        return 0.0;

    double beats = static_cast<double>(strip->getBeatsPerLoop());
    if (!std::isfinite(beats) || beats <= 0.0)
        beats = static_cast<double>(juce::jmax(1, strip->getRecordingBars()) * 4);

    return juce::jlimit(0.25, 256.0, beats);
}

double SceneScheduler::computeLongestStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    double longestBeats = 0.0;

    if (processor.audioEngine != nullptr)
        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            longestBeats = juce::jmax(longestBeats, computeStripSceneSequenceLengthBeats(processor, stripIndex));
    return longestBeats;
}

double SceneScheduler::computeLongestPatternSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    double longestBeats = 0.0;

    if (processor.audioEngine != nullptr)
    {
        for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
        {
            auto* pattern = processor.audioEngine->getPattern(patternIndex);
            if (pattern == nullptr)
                continue;
            if (pattern->getEventCount() <= 0 && !pattern->isPlaying() && !pattern->isRecording())
                continue;

            longestBeats = juce::jmax(
                longestBeats,
                juce::jlimit(1.0, 256.0, static_cast<double>(pattern->getLengthInBeats())));
        }
    }

    return longestBeats;
}

double SceneScheduler::getResolvedSceneLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const double longestStripBeats = computeLongestStripSceneSequenceLengthBeats(processor);
    const double longestPatternBeats = computeLongestPatternSceneSequenceLengthBeats(processor);

    double resolvedBeats = 0.0;
    switch (sanitizeSceneLengthMode(getSceneLengthModeIndex(processor, sceneSlot)))
    {
        case SceneLengthMode::LongestStrip:
            resolvedBeats = longestStripBeats > 0.0 ? longestStripBeats : longestPatternBeats;
            break;
        case SceneLengthMode::LongestPattern:
            resolvedBeats = longestPatternBeats > 0.0 ? longestPatternBeats : longestStripBeats;
            break;
        case SceneLengthMode::ManualBars:
            resolvedBeats = static_cast<double>(getSceneManualBars(processor, sceneSlot)) * 4.0;
            break;
        case SceneLengthMode::AnchorStrip:
            resolvedBeats = computeStripSceneSequenceLengthBeats(processor, getSceneAnchorStrip(processor, sceneSlot));
            if (resolvedBeats <= 0.0)
                resolvedBeats = longestStripBeats > 0.0 ? longestStripBeats : longestPatternBeats;
            break;
    }

    return juce::jlimit(4.0, 4096.0, resolvedBeats);
}

double SceneScheduler::getSceneAdvanceLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const double baseBeats = getResolvedSceneLengthBeats(processor, clampedSlot);
    if (getSceneLengthModeIndex(processor, clampedSlot) == static_cast<int>(SceneLengthMode::ManualBars))
        return baseBeats;

    return juce::jlimit(
        1.0,
        4096.0,
        baseBeats * static_cast<double>(getSceneRepeatCount(processor, clampedSlot)));
}

bool SceneScheduler::persistSceneTimingForSlot(MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, clampedSlot);
    if (!PresetStore::presetExists(storageIndex))
        return false;

    const bool updated = PresetStore::updatePresetAuxState(
        storageIndex,
        [&processor, clampedSlot]()
        {
            return createSceneChainStateXml(processor, clampedSlot);
        });
    if (updated)
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return updated;
}

int SceneScheduler::getSceneSequenceStepIndex(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    for (size_t i = 0; i < processor.sceneSequenceSlots.size(); ++i)
        if (processor.sceneSequenceSlots[i] == clampedSlot)
            return static_cast<int>(i);
    return -1;
}

int SceneScheduler::getQueuedSceneSlot(const MlrVSTAudioProcessor& processor)
{
    const int queuedApplySlot = processor.pendingSceneApplySlot.load(std::memory_order_acquire);
    if (queuedApplySlot >= 0)
        return juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, queuedApplySlot);
    if (processor.pendingSceneRecall.active)
        return juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.pendingSceneRecall.sceneSlot);
    return -1;
}

juce::String SceneScheduler::getSceneSequenceSummaryText(const MlrVSTAudioProcessor& processor)
{
    const int activeSlot = processor.getActiveSceneSlot();
    const int queuedSlot = getQueuedSceneSlot(processor);
    juce::String changeModeLabel;
    switch (sanitizeSceneRecallMode(getSceneRecallModeIndex(processor)))
    {
        case SceneRecallMode::QuantizeGrid: changeModeLabel = "Grid"; break;
        case SceneRecallMode::PatternEnd:   changeModeLabel = "Pattern End"; break;
        case SceneRecallMode::SceneEnd:     changeModeLabel = "Scene End"; break;
        case SceneRecallMode::Manual:       changeModeLabel = "Manual"; break;
    }
    if (!processor.sceneSequenceActive || processor.sceneSequenceSlots.size() < 2)
    {
        juce::String summary = "Chain: off | Change: " + changeModeLabel
            + " | Active: S" + juce::String(activeSlot + 1);
        if (queuedSlot >= 0)
            summary << " | Next: S" << juce::String(queuedSlot + 1);
        return summary;
    }

    juce::String summary = "Chain: ";
    for (size_t i = 0; i < processor.sceneSequenceSlots.size(); ++i)
    {
        if (i > 0)
            summary << " -> ";
        summary << "S" << juce::String(processor.sceneSequenceSlots[i] + 1);
    }
    summary << " | Change: " << changeModeLabel;
    summary << " | Active: S" << juce::String(activeSlot + 1);
    if (queuedSlot >= 0)
        summary << " | Next: S" << juce::String(queuedSlot + 1);
    return summary;
}

std::unique_ptr<juce::XmlElement> SceneScheduler::createSceneChainStateXml(const MlrVSTAudioProcessor& processor,
                                                                           int sceneSlotOverride)
{
    auto xml = std::make_unique<juce::XmlElement>("SceneChainState");
    if (sceneSlotOverride >= 0)
    {
        const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlotOverride);
        xml->setAttribute("sceneSlot", clampedSlot);
        xml->setAttribute("repeatCount", getSceneRepeatCount(processor, clampedSlot));
        xml->setAttribute("lengthMode", getSceneLengthModeIndex(processor, clampedSlot));
        xml->setAttribute("manualBars", getSceneManualBars(processor, clampedSlot));
        xml->setAttribute("anchorStrip", getSceneAnchorStrip(processor, clampedSlot));
        return xml;
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        xml->setAttribute("sceneRepeat" + juce::String(sceneSlot), getSceneRepeatCount(processor, sceneSlot));
        xml->setAttribute("sceneLengthMode" + juce::String(sceneSlot), getSceneLengthModeIndex(processor, sceneSlot));
        xml->setAttribute("sceneManualBars" + juce::String(sceneSlot), getSceneManualBars(processor, sceneSlot));
        xml->setAttribute("sceneAnchorStrip" + juce::String(sceneSlot), getSceneAnchorStrip(processor, sceneSlot));
    }
    return xml;
}

void SceneScheduler::applySceneChainStateXml(MlrVSTAudioProcessor& processor,
                                             const juce::XmlElement* xml,
                                             int sceneSlotOverride)
{
    if (xml == nullptr || !xml->hasTagName("SceneChainState"))
        return;

    if (sceneSlotOverride >= 0)
    {
        const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlotOverride);
        const int repeatCount = xml->hasAttribute("repeatCount")
            ? xml->getIntAttribute("repeatCount", getSceneRepeatCount(processor, clampedSlot))
            : xml->getIntAttribute("sceneRepeat" + juce::String(clampedSlot), getSceneRepeatCount(processor, clampedSlot));
        const int lengthMode = xml->hasAttribute("lengthMode")
            ? xml->getIntAttribute("lengthMode", getSceneLengthModeIndex(processor, clampedSlot))
            : xml->getIntAttribute("sceneLengthMode" + juce::String(clampedSlot), getSceneLengthModeIndex(processor, clampedSlot));
        const int manualBars = xml->hasAttribute("manualBars")
            ? xml->getIntAttribute("manualBars", getSceneManualBars(processor, clampedSlot))
            : xml->getIntAttribute("sceneManualBars" + juce::String(clampedSlot), getSceneManualBars(processor, clampedSlot));
        const int anchorStrip = xml->hasAttribute("anchorStrip")
            ? xml->getIntAttribute("anchorStrip", getSceneAnchorStrip(processor, clampedSlot))
            : xml->getIntAttribute("sceneAnchorStrip" + juce::String(clampedSlot), getSceneAnchorStrip(processor, clampedSlot));
        setSceneRepeatCount(processor, clampedSlot, repeatCount);
        setSceneLengthModeIndex(processor, clampedSlot, lengthMode);
        setSceneManualBars(processor, clampedSlot, manualBars);
        setSceneAnchorStrip(processor, clampedSlot, anchorStrip);
        return;
    }

    bool anyApplied = false;
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        const auto repeatAttr = "sceneRepeat" + juce::String(sceneSlot);
        const auto lengthAttr = "sceneLengthMode" + juce::String(sceneSlot);
        const auto barsAttr = "sceneManualBars" + juce::String(sceneSlot);
        const auto anchorAttr = "sceneAnchorStrip" + juce::String(sceneSlot);
        const bool hasSceneAttributes = xml->hasAttribute(repeatAttr)
            || xml->hasAttribute(lengthAttr)
            || xml->hasAttribute(barsAttr)
            || xml->hasAttribute(anchorAttr);
        if (!hasSceneAttributes)
            continue;

        if (xml->hasAttribute(repeatAttr))
            setSceneRepeatCount(processor, sceneSlot, xml->getIntAttribute(repeatAttr, getSceneRepeatCount(processor, sceneSlot)));
        if (xml->hasAttribute(lengthAttr))
            setSceneLengthModeIndex(processor, sceneSlot, xml->getIntAttribute(lengthAttr, getSceneLengthModeIndex(processor, sceneSlot)));
        if (xml->hasAttribute(barsAttr))
            setSceneManualBars(processor, sceneSlot, xml->getIntAttribute(barsAttr, getSceneManualBars(processor, sceneSlot)));
        if (xml->hasAttribute(anchorAttr))
            setSceneAnchorStrip(processor, sceneSlot, xml->getIntAttribute(anchorAttr, getSceneAnchorStrip(processor, sceneSlot)));
        anyApplied = true;
    }

    if (!anyApplied && xml->hasAttribute("sceneSlot"))
    {
        const int slot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, xml->getIntAttribute("sceneSlot", 0));
        setSceneRepeatCount(processor, slot, xml->getIntAttribute("repeatCount", getSceneRepeatCount(processor, slot)));
        setSceneLengthModeIndex(processor, slot, xml->getIntAttribute("lengthMode", getSceneLengthModeIndex(processor, slot)));
        setSceneManualBars(processor, slot, xml->getIntAttribute("manualBars", getSceneManualBars(processor, slot)));
        setSceneAnchorStrip(processor, slot, xml->getIntAttribute("anchorStrip", getSceneAnchorStrip(processor, slot)));
    }
}

double SceneScheduler::computeCurrentSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    return getSceneAdvanceLengthBeats(processor, processor.activeSceneSlot);
}

double SceneScheduler::computeNextScenePatternEndPpq(const MlrVSTAudioProcessor& processor,
                                                     int sceneSlot,
                                                     double currentPpq,
                                                     double cycleBeats,
                                                     uint64_t* outPhaseSignature)
{
    if (processor.audioEngine == nullptr
        || !std::isfinite(currentPpq)
        || !std::isfinite(cycleBeats)
        || cycleBeats <= 0.0)
    {
        if (outPhaseSignature != nullptr)
            *outPhaseSignature = 0;
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int clampedSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const auto lengthMode = sanitizeSceneLengthMode(getSceneLengthModeIndex(processor, clampedSceneSlot));
    const int anchorStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, getSceneAnchorStrip(processor, clampedSceneSlot));

    bool foundCandidate = false;
    double nextPatternEndPpq = std::numeric_limits<double>::quiet_NaN();
    uint64_t phaseSignature = 0xcbf29ce484222325ull;

    auto combineSignature = [&phaseSignature](uint64_t value)
    {
        phaseSignature ^= value + 0x9e3779b97f4a7c15ull + (phaseSignature << 6) + (phaseSignature >> 2);
    };

    auto accumulateStripBoundary = [&](int stripIndex)
    {
        if (stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
            return;

        auto* strip = processor.audioEngine->getStrip(stripIndex);
        if (strip == nullptr || !strip->isPlaying() || !strip->isPpqTimelineAnchored())
            return;

        const auto playMode = strip->getPlayMode();
        if (playMode == EnhancedAudioStrip::PlayMode::Step)
            return;

        const bool hasStripAudio = playMode == EnhancedAudioStrip::PlayMode::Sample
            ? processor.hasSampleModeAudio(stripIndex)
            : strip->hasAudio();
        if (!hasStripAudio)
            return;

        const double triggerPpq = strip->getLastTriggerPPQ();
        if (!std::isfinite(triggerPpq) || triggerPpq < 0.0)
            return;

        uint64_t triggerBits = 0;
        static_assert(sizeof(triggerBits) == sizeof(triggerPpq), "Unexpected double size");
        std::memcpy(&triggerBits, &triggerPpq, sizeof(triggerBits));
        combineSignature(static_cast<uint64_t>(stripIndex + 1));
        combineSignature(static_cast<uint64_t>(playMode));
        combineSignature(triggerBits);

        const double stripCycleBeats = juce::jmax(
            cycleBeats,
            computeStripSceneSequenceLengthBeats(processor, stripIndex));
        if (!std::isfinite(stripCycleBeats) || stripCycleBeats <= 0.0)
            return;

        double nextBoundary = triggerPpq + stripCycleBeats;
        if (nextBoundary <= currentPpq + 1.0e-9)
        {
            const double elapsed = juce::jmax(0.0, currentPpq - triggerPpq);
            const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, stripCycleBeats));
            nextBoundary = triggerPpq + ((completedCycles + 1.0) * stripCycleBeats);
        }

        nextPatternEndPpq = foundCandidate ? juce::jmax(nextPatternEndPpq, nextBoundary) : nextBoundary;
        foundCandidate = true;
    };

    if (lengthMode == SceneLengthMode::AnchorStrip)
        accumulateStripBoundary(anchorStrip);

    if (!foundCandidate)
    {
        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            accumulateStripBoundary(stripIndex);
    }

    if (outPhaseSignature != nullptr)
        *outPhaseSignature = foundCandidate ? phaseSignature : 0;

    return foundCandidate ? nextPatternEndPpq : std::numeric_limits<double>::quiet_NaN();
}

void SceneScheduler::armNextSceneInSequence(MlrVSTAudioProcessor& processor,
                                            int mainPresetIndex,
                                            int currentSceneSlot,
                                            double sceneStartPpq)
{
    if (!processor.sceneSequenceActive || processor.sceneSequenceSlots.size() < 2)
    {
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.sceneSequenceStartPpqValid = false;
        return;
    }

    processor.sceneSequenceStartPpqValid = std::isfinite(sceneStartPpq);
    processor.sceneSequenceStartPpq = processor.sceneSequenceStartPpqValid ? sceneStartPpq : 0.0;

    int currentIndex = -1;
    for (size_t i = 0; i < processor.sceneSequenceSlots.size(); ++i)
    {
        if (processor.sceneSequenceSlots[i] == currentSceneSlot)
        {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    if (currentIndex < 0)
        currentIndex = 0;

    const int nextSlot = juce::jlimit(
        0,
        MlrVSTAudioProcessor::SceneSlots - 1,
        processor.sceneSequenceSlots[(static_cast<size_t>(currentIndex) + 1u) % processor.sceneSequenceSlots.size()]);

    processor.pendingSceneRecall.active = true;
    processor.pendingSceneRecall.sequenceDriven = true;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.mainPresetIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    processor.pendingSceneRecall.sceneSlot = nextSlot;
    processor.pendingSceneRecall.targetPpq = 0.0;
    processor.pendingSceneRecall.intervalBeats = 0.0;
    processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
    processor.pendingSceneRecall.patternEndPhaseSignature = 0;

    if (!processor.isTimerRunning())
        processor.startTimer(MlrVSTAudioProcessor::kGridRefreshMs);
}

void SceneScheduler::setSceneModeEnabled(MlrVSTAudioProcessor& processor, bool enabled)
{
    if (auto* param = processor.parameters.getParameter("sceneMode"))
    {
        const bool currentParamState = param->getValue() > 0.5f;
        if (currentParamState != enabled)
            param->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
    }

    applySceneModeState(processor, enabled);
}

void SceneScheduler::captureSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor)
{
    if (processor.sceneModeGroupSnapshot.valid || processor.audioEngine == nullptr)
        return;

    processor.sceneModeGroupSnapshot.valid = true;
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        auto* strip = processor.audioEngine->getStrip(stripIndex);
        processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)] =
            strip != nullptr ? strip->getGroup() : -1;
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        auto* group = processor.audioEngine->getGroup(groupIndex);
        processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)] =
            group != nullptr ? group->getVolume() : 1.0f;
        processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)] =
            group != nullptr ? group->isMuted() : false;
    }
}

void SceneScheduler::restoreSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor)
{
    if (!processor.sceneModeGroupSnapshot.valid || processor.audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        processor.audioEngine->assignStripToGroup(
            stripIndex,
            processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)]);
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        if (auto* group = processor.audioEngine->getGroup(groupIndex))
        {
            group->setVolume(processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)]);
            group->setMuted(processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)]);
        }
    }

    processor.sceneModeGroupSnapshot.valid = false;
}

void SceneScheduler::clearAllStripGroupsForSceneMode(MlrVSTAudioProcessor& processor)
{
    if (processor.audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
        processor.audioEngine->assignStripToGroup(stripIndex, -1);
}

void SceneScheduler::applySceneModeState(MlrVSTAudioProcessor& processor, bool enabled)
{
    const bool previousEnabled = processor.sceneModeEnabled.exchange(enabled ? 1 : 0, std::memory_order_acq_rel) != 0;
    if (previousEnabled == enabled)
    {
        if (processor.audioEngine != nullptr)
            processor.audioEngine->setPatternRecorderIgnoreGroups(enabled);
        return;
    }

    if (processor.audioEngine != nullptr)
        processor.audioEngine->setPatternRecorderIgnoreGroups(enabled);

    if (enabled)
    {
        captureSceneModeGroupSnapshot(processor);
        clearAllStripGroupsForSceneMode(processor);
        if (processor.controlModeActive && processor.currentControlMode == MlrVSTAudioProcessor::ControlMode::GroupAssign)
        {
            processor.currentControlMode = MlrVSTAudioProcessor::ControlMode::Normal;
            processor.controlModeActive = false;
        }
    }
    else
    {
        restoreSceneModeGroupSnapshot(processor);
    }

    processor.pendingSceneRecall.active = false;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.sequenceDriven = false;
    processor.sceneSequenceActive = false;
    processor.sceneSequenceSlots.clear();
    processor.sceneSequenceStartPpqValid = false;
    processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
    processor.scenePadHeld.fill(false);
    processor.scenePadHoldDeleteTriggered.fill(false);
    processor.scenePadPressStartMs.fill(0);
    processor.scenePadActionBurstUntilMs.fill(0);
    processor.scenePadLastTapMs.fill(0);
    processor.sceneCopySourceSlot = -1;
    processor.sceneCopyMainPresetIndex = 0;

    processor.updateMonomeLEDs();
    processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void SceneScheduler::syncSceneModeFromParameters(MlrVSTAudioProcessor& processor)
{
    const bool desiredState = processor.sceneModeParam != nullptr
        && processor.sceneModeParam->load(std::memory_order_acquire) > 0.5f;
    if (desiredState != processor.isSceneModeEnabled())
        applySceneModeState(processor, desiredState);
}

bool SceneScheduler::saveSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot)
{
    if (!processor.audioEngine)
        return false;

    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    const bool saved = PresetStore::savePreset(storageIndex,
                                               MlrVSTAudioProcessor::MaxStrips,
                                               processor.audioEngine.get(),
                                               processor.parameters,
                                               processor.currentStripFiles.data(),
                                               processor.recentLoopDirectories.data(),
                                               processor.recentStepDirectories.data(),
                                               processor.recentFlipDirectories.data(),
                                               [&processor](int stripIndex)
                                               {
                                                   return processor.createFlipPresetStateXml(stripIndex);
                                               },
                                               [&processor](int stripIndex)
                                               {
                                                   return processor.createLoopPitchPresetStateXml(stripIndex);
                                               },
                                               [&processor, sceneSlot]()
                                               {
                                                   return createSceneChainStateXml(processor, sceneSlot);
                                               });
    if (saved)
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return saved;
}

bool SceneScheduler::copySceneForMainPreset(MlrVSTAudioProcessor& processor,
                                            int mainPresetIndex,
                                            int sourceSceneSlot,
                                            int destSceneSlot)
{
    const int clampedMain = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSource = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sourceSceneSlot);
    const int clampedDest = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, destSceneSlot);
    const int sourceStorageIndex = processor.getSceneStoragePresetIndex(clampedMain, clampedSource);
    const int destStorageIndex = processor.getSceneStoragePresetIndex(clampedMain, clampedDest);
    if (sourceStorageIndex == destStorageIndex)
        return true;

    const bool sourceExists = PresetStore::presetExists(sourceStorageIndex);
    if (!sourceExists)
    {
        if (PresetStore::presetExists(destStorageIndex))
        {
            const bool deleted = PresetStore::deletePreset(destStorageIndex);
            if (deleted)
                processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
            return deleted;
        }
        return true;
    }

    const bool copied = PresetStore::copyPreset(sourceStorageIndex, destStorageIndex);
    if (copied)
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return copied;
}

bool SceneScheduler::deleteSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot)
{
    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    if (!PresetStore::presetExists(storageIndex))
        return true;

    const bool deleted = PresetStore::deletePreset(storageIndex);
    if (deleted)
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return deleted;
}

bool SceneScheduler::captureSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    processor.activeSceneMainPresetIndex = mainPresetIndex;

    const bool saved = saveSceneForMainPreset(processor, mainPresetIndex, clampedSlot);
    if (saved)
        processor.updateMonomeLEDs();
    return saved;
}

bool SceneScheduler::insertSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot, bool insertAfter)
{
    const int selectedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int insertSlot = insertAfter ? (selectedSlot + 1) : selectedSlot;
    if (insertSlot < 0 || insertSlot >= MlrVSTAudioProcessor::SceneSlots)
        return false;

    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    processor.activeSceneMainPresetIndex = mainPresetIndex;

    bool storageChanged = false;
    for (int destSlot = MlrVSTAudioProcessor::SceneSlots - 1; destSlot > insertSlot; --destSlot)
    {
        if (!copySceneForMainPreset(processor, mainPresetIndex, destSlot - 1, destSlot))
            return false;

        setSceneRepeatCount(processor, destSlot, getSceneRepeatCount(processor, destSlot - 1));
        setSceneLengthModeIndex(processor, destSlot, getSceneLengthModeIndex(processor, destSlot - 1));
        setSceneManualBars(processor, destSlot, getSceneManualBars(processor, destSlot - 1));
        setSceneAnchorStrip(processor, destSlot, getSceneAnchorStrip(processor, destSlot - 1));
        storageChanged = true;
    }

    const bool captured = captureSceneSlot(processor, insertSlot);
    if (!captured && storageChanged)
    {
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        processor.updateMonomeLEDs();
    }

    return captured;
}

void SceneScheduler::requestSceneRecallQuantized(MlrVSTAudioProcessor& processor,
                                                 int mainPresetIndex,
                                                 int sceneSlot,
                                                 bool sequenceDriven)
{
    if (!processor.audioEngine)
        return;

    processor.activeSceneMainPresetIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const bool directManualRecall = !sequenceDriven && getSceneRecallModeIndex(processor) == static_cast<int>(SceneRecallMode::Manual);
    const bool hostTransportPlaying = processor.isHostTransportPlaying();
    const bool immediateRecall = !sequenceDriven && (!hostTransportPlaying || directManualRecall);

    processor.pendingSceneRecall.active = !immediateRecall;
    processor.pendingSceneRecall.sequenceDriven = sequenceDriven;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.mainPresetIndex = processor.activeSceneMainPresetIndex;
    processor.pendingSceneRecall.sceneSlot = clampedSceneSlot;
    processor.pendingSceneRecall.targetPpq = 0.0;
    processor.pendingSceneRecall.intervalBeats = 4.0;
    processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
    processor.pendingSceneRecall.patternEndPhaseSignature = 0;
    if (!sequenceDriven)
        processor.sceneSequenceStartPpqValid = false;

    if (immediateRecall)
    {
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = std::numeric_limits<double>::quiet_NaN();
        const bool hasHostSync = processor.getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot);
        processor.pendingSceneApplyTargetPpq.store(hasHostSync ? hostPpqSnapshot : -1.0, std::memory_order_release);
        processor.pendingSceneApplyTargetTempo.store(hasHostSync ? hostTempoSnapshot : 120.0, std::memory_order_release);
        processor.pendingSceneApplyTargetSample.store(processor.audioEngine->getGlobalSampleCount(), std::memory_order_release);
        processor.pendingSceneApplyMainPreset.store(processor.activeSceneMainPresetIndex, std::memory_order_release);
        processor.pendingSceneApplySlot.store(clampedSceneSlot, std::memory_order_release);
        processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    }

    processor.refreshUtilityTimerCadence();
}

double SceneScheduler::getSceneRecallIntervalBeats(const MlrVSTAudioProcessor& processor)
{
    const int quantizeDivision = juce::jmax(1, processor.getQuantizeDivision());
    return juce::jlimit(1.0 / 64.0, 256.0, 4.0 / static_cast<double>(quantizeDivision));
}

void SceneScheduler::updateSceneQuantizedRecall(MlrVSTAudioProcessor& processor,
                                                const juce::AudioPlayHead::PositionInfo& posInfo,
                                                int numSamples)
{
    if (!processor.pendingSceneRecall.active)
        return;

    if (processor.pendingSceneRecall.sequenceDriven
        && (!processor.sceneSequenceActive || processor.sceneSequenceSlots.size() < 2))
    {
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        return;
    }

    if (processor.pendingSceneApplySlot.load(std::memory_order_acquire) >= 0)
        return;

    if (!posInfo.getIsPlaying())
    {
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
        processor.pendingSceneRecall.patternEndPhaseSignature = 0;
        return;
    }

    const auto ppqOpt = posInfo.getPpqPosition();
    const auto bpmOpt = posInfo.getBpm();
    if (!ppqOpt.hasValue() || !bpmOpt.hasValue()
        || !std::isfinite(*ppqOpt) || !std::isfinite(*bpmOpt)
        || *bpmOpt <= 0.0 || processor.currentSampleRate <= 1.0)
    {
        return;
    }

    const double currentPpq = *ppqOpt;
    const auto sceneRecallMode = sanitizeSceneRecallMode(getSceneRecallModeIndex(processor));
    const bool patternEndEnabled = sceneRecallMode == SceneRecallMode::PatternEnd;
    const bool sceneEndEnabled = sceneRecallMode == SceneRecallMode::SceneEnd;
    const bool manualSceneChange = sceneRecallMode == SceneRecallMode::Manual;
    const int currentSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.activeSceneSlot);
    const double currentSceneDurationBeats = juce::jlimit(
        0.25,
        4096.0,
        computeCurrentSceneSequenceLengthBeats(processor));
    uint64_t phaseAlignedPatternEndSignature = 0;
    const double phaseAlignedPatternEndPpq = patternEndEnabled
        ? computeNextScenePatternEndPpq(
              processor,
              currentSceneSlot,
              currentPpq,
              currentSceneDurationBeats,
              &phaseAlignedPatternEndSignature)
        : std::numeric_limits<double>::quiet_NaN();
    const bool phaseAlignedTimingReady = patternEndEnabled && std::isfinite(phaseAlignedPatternEndPpq);
    const bool sequenceTimingReady = sceneEndEnabled
        && processor.pendingSceneRecall.sequenceDriven
        && processor.sceneSequenceStartPpqValid
        && std::isfinite(processor.sceneSequenceStartPpq);
    const bool manualSceneEndTimingReady = sceneEndEnabled
        && !processor.pendingSceneRecall.sequenceDriven
        && processor.activeSceneStartPpqValid
        && std::isfinite(processor.activeSceneStartPpq);
    const bool useSceneDurationTiming = phaseAlignedTimingReady || sequenceTimingReady || manualSceneEndTimingReady;
    double intervalBeatsNow = getSceneRecallIntervalBeats(processor);
    if (useSceneDurationTiming)
        intervalBeatsNow = currentSceneDurationBeats;

    if (processor.pendingSceneRecall.targetResolved
        && std::abs(intervalBeatsNow - processor.pendingSceneRecall.intervalBeats) > 1.0e-9)
    {
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.targetPpq = 0.0;
    }

    if (patternEndEnabled)
    {
        if (phaseAlignedTimingReady)
        {
            if (!processor.pendingSceneRecall.patternEndPhaseSignatureValid
                || processor.pendingSceneRecall.patternEndPhaseSignature != phaseAlignedPatternEndSignature)
            {
                processor.pendingSceneRecall.targetResolved = false;
                processor.pendingSceneRecall.targetPpq = 0.0;
            }

            processor.pendingSceneRecall.patternEndPhaseSignatureValid = true;
            processor.pendingSceneRecall.patternEndPhaseSignature = phaseAlignedPatternEndSignature;
        }
        else if (processor.pendingSceneRecall.patternEndPhaseSignatureValid)
        {
            processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
            processor.pendingSceneRecall.patternEndPhaseSignature = 0;
            processor.pendingSceneRecall.targetResolved = false;
            processor.pendingSceneRecall.targetPpq = 0.0;
        }

        // Pattern End must never silently fall back to grid timing.
        // If we do not yet have a valid loop-phase boundary for the active scene,
        // keep waiting instead of arming an early scene change.
        if (!phaseAlignedTimingReady)
        {
            processor.pendingSceneRecall.targetResolved = false;
            processor.pendingSceneRecall.targetPpq = 0.0;
            return;
        }
    }
    else if (processor.pendingSceneRecall.patternEndPhaseSignatureValid)
    {
        processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
        processor.pendingSceneRecall.patternEndPhaseSignature = 0;
    }

    if (manualSceneChange && processor.pendingSceneRecall.sequenceDriven)
    {
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        return;
    }

    if (!processor.pendingSceneRecall.targetResolved)
    {
        processor.pendingSceneRecall.intervalBeats = intervalBeatsNow;
        if (manualSceneChange)
        {
            processor.pendingSceneRecall.targetPpq = currentPpq;
        }
        else if (phaseAlignedTimingReady)
        {
            processor.pendingSceneRecall.targetPpq = phaseAlignedPatternEndPpq;
        }
        else if (useSceneDurationTiming)
        {
            const double sceneStartReference = sequenceTimingReady ? processor.sceneSequenceStartPpq : processor.activeSceneStartPpq;
            double nextTarget = sceneStartReference + intervalBeatsNow;
            if (nextTarget <= currentPpq + 1.0e-9)
            {
                const double elapsed = juce::jmax(0.0, currentPpq - sceneStartReference);
                const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, intervalBeatsNow));
                nextTarget = sceneStartReference + ((completedCycles + 1.0) * intervalBeatsNow);
            }
            processor.pendingSceneRecall.targetPpq = nextTarget;
        }
        else
        {
            double nextBoundary = std::ceil(currentPpq / intervalBeatsNow) * intervalBeatsNow;
            if (nextBoundary <= currentPpq + 1.0e-9)
                nextBoundary += intervalBeatsNow;
            processor.pendingSceneRecall.targetPpq = std::round(nextBoundary / intervalBeatsNow) * intervalBeatsNow;
        }
        processor.pendingSceneRecall.targetResolved = true;
    }

    const double ppqPerSecond = *bpmOpt / 60.0;
    const double ppqPerSample = ppqPerSecond / processor.currentSampleRate;
    const double blockEndPpq = currentPpq + (ppqPerSample * static_cast<double>(juce::jmax(1, numSamples)));
    if (blockEndPpq + 1.0e-9 < processor.pendingSceneRecall.targetPpq)
        return;

    const double targetPpq = processor.pendingSceneRecall.targetPpq;
    const double samplesToTarget = (targetPpq - currentPpq) / juce::jmax(1.0e-12, ppqPerSample);
    const int sampleOffset = juce::jlimit(
        0,
        juce::jmax(0, numSamples - 1),
        static_cast<int>(std::llround(samplesToTarget)));
    const int64_t targetGlobalSample = processor.audioEngine != nullptr
        ? processor.audioEngine->getGlobalSampleCount() + static_cast<int64_t>(sampleOffset)
        : -1;

    processor.pendingSceneApplyTargetPpq.store(targetPpq, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(*bpmOpt, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(targetGlobalSample, std::memory_order_release);
    processor.pendingSceneApplyMainPreset.store(processor.pendingSceneRecall.mainPresetIndex, std::memory_order_release);
    processor.pendingSceneApplySlot.store(processor.pendingSceneRecall.sceneSlot, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(processor.pendingSceneRecall.sequenceDriven ? 1 : 0, std::memory_order_release);
    processor.pendingSceneRecall.active = false;
    processor.pendingSceneRecall.targetResolved = false;
}

void SceneScheduler::processPendingSceneApply(MlrVSTAudioProcessor& processor)
{
    const int queuedSlot = processor.pendingSceneApplySlot.exchange(-1, std::memory_order_acq_rel);
    if (queuedSlot < 0)
        return;

    const int queuedMain = processor.pendingSceneApplyMainPreset.exchange(-1, std::memory_order_acq_rel);
    const bool queuedSequenceDriven = processor.pendingSceneApplySequenceDriven.exchange(0, std::memory_order_acq_rel) != 0;
    const double queuedTargetPpq = processor.pendingSceneApplyTargetPpq.exchange(-1.0, std::memory_order_acq_rel);
    const double queuedTargetTempo = processor.pendingSceneApplyTargetTempo.exchange(120.0, std::memory_order_acq_rel);
    const int64_t queuedTargetSample = processor.pendingSceneApplyTargetSample.exchange(-1, std::memory_order_acq_rel);

    const int clampedMain = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, queuedMain);
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, queuedSlot);
    const bool queuedTimingValid = std::isfinite(queuedTargetPpq)
        && std::isfinite(queuedTargetTempo)
        && queuedTargetPpq >= 0.0
        && queuedTargetTempo > 0.0;
    const bool preserveSceneSequence = processor.sceneSequenceActive && processor.sceneSequenceSlots.size() >= 2;
    const bool effectiveSequenceDriven = queuedSequenceDriven || preserveSceneSequence;
    const auto preservedSceneSequenceSlots = preserveSceneSequence ? processor.sceneSequenceSlots : std::vector<int>{};
    const bool hostTransportPlaying = processor.isHostTransportPlaying();
    auto requeuePendingApply = [&]()
    {
        processor.pendingSceneApplyMainPreset.store(clampedMain, std::memory_order_release);
        processor.pendingSceneApplySlot.store(clampedSlot, std::memory_order_release);
        processor.pendingSceneApplySequenceDriven.store(effectiveSequenceDriven ? 1 : 0, std::memory_order_release);
        processor.pendingSceneApplyTargetPpq.store(queuedTargetPpq, std::memory_order_release);
        processor.pendingSceneApplyTargetTempo.store(queuedTargetTempo, std::memory_order_release);
        processor.pendingSceneApplyTargetSample.store(queuedTargetSample, std::memory_order_release);
    };

    const int64_t currentGlobalSample = processor.audioEngine != nullptr ? processor.audioEngine->getGlobalSampleCount() : -1;
    if (queuedTargetSample >= 0
        && currentGlobalSample >= 0
        && (hostTransportPlaying || effectiveSequenceDriven)
        && currentGlobalSample + 1 < queuedTargetSample)
    {
        requeuePendingApply();
        return;
    }

    double hostPpqSnapshot = processor.audioEngine ? processor.audioEngine->getTimelineBeat() : 0.0;
    double hostTempoSnapshot = processor.audioEngine ? juce::jmax(1.0, processor.audioEngine->getCurrentTempo()) : 120.0;
    const bool hasHostSync = processor.getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot)
        && std::isfinite(hostPpqSnapshot)
        && std::isfinite(hostTempoSnapshot)
        && hostTempoSnapshot > 0.0;

    if (effectiveSequenceDriven && !hostTransportPlaying)
    {
        requeuePendingApply();
        return;
    }

    if (!queuedTimingValid && !hasHostSync && effectiveSequenceDriven)
    {
        requeuePendingApply();
        return;
    }

    const double appliedPpq = queuedTimingValid
        ? queuedTargetPpq
        : (hasHostSync ? hostPpqSnapshot : std::numeric_limits<double>::quiet_NaN());
    const double appliedTempo = queuedTimingValid
        ? queuedTargetTempo
        : (hasHostSync ? hostTempoSnapshot : std::numeric_limits<double>::quiet_NaN());
    const int64_t appliedGlobalSample = queuedTargetSample >= 0
        ? queuedTargetSample
        : (processor.audioEngine != nullptr ? processor.audioEngine->getGlobalSampleCount() : -1);

    if (processor.sceneSlotExistsForMainPreset(clampedMain, clampedSlot))
    {
        performSceneLoad(processor,
                         clampedMain,
                         clampedSlot,
                         appliedPpq,
                         appliedTempo,
                         appliedGlobalSample);
    }
    else
    {
        performEmptySceneLoad(processor);
    }

    if (preserveSceneSequence)
    {
        processor.sceneSequenceActive = true;
        processor.sceneSequenceSlots = preservedSceneSequenceSlots;
    }

    processor.activeSceneMainPresetIndex = clampedMain;
    processor.activeSceneSlot = clampedSlot;
    const double appliedSceneStartPpq = appliedPpq;
    processor.activeSceneStartPpqValid = std::isfinite(appliedSceneStartPpq);
    processor.activeSceneStartPpq = processor.activeSceneStartPpqValid ? appliedSceneStartPpq : 0.0;
    if (PresetStore::presetExists(clampedMain))
        processor.loadedPresetIndex = clampedMain;
    if (effectiveSequenceDriven && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::Manual))
        armNextSceneInSequence(processor, clampedMain, clampedSlot, appliedPpq);
    else
        processor.sceneSequenceStartPpqValid = false;
    processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    processor.updateMonomeLEDs();
}

void SceneScheduler::performEmptySceneLoad(MlrVSTAudioProcessor& processor)
{
    ScopedSuspendProcessing scopedSuspend(processor);

    const auto preservedScenePadHeld = processor.scenePadHeld;
    const auto preservedScenePadHoldDeleteTriggered = processor.scenePadHoldDeleteTriggered;
    const auto preservedScenePadPressStartMs = processor.scenePadPressStartMs;
    const auto preservedScenePadActionBurstUntilMs = processor.scenePadActionBurstUntilMs;
    const auto preservedScenePadLastTapMs = processor.scenePadLastTapMs;
    const int preservedSceneCopySourceSlot = processor.sceneCopySourceSlot;
    const int preservedSceneCopyMainPresetIndex = processor.sceneCopyMainPresetIndex;

    processor.resetRuntimePresetStateToDefaults();
    processor.scenePadHeld = preservedScenePadHeld;
    processor.scenePadHoldDeleteTriggered = preservedScenePadHoldDeleteTriggered;
    processor.scenePadPressStartMs = preservedScenePadPressStartMs;
    processor.scenePadActionBurstUntilMs = preservedScenePadActionBurstUntilMs;
    processor.scenePadLastTapMs = preservedScenePadLastTapMs;
    processor.sceneCopySourceSlot = preservedSceneCopySourceSlot;
    processor.sceneCopyMainPresetIndex = preservedSceneCopyMainPresetIndex;
    for (auto& f : processor.currentStripFiles)
        f = juce::File();

    const int blendSamples = juce::jmax(12, static_cast<int>(std::round(processor.currentSampleRate * 0.00035)));
    processor.sceneRecallBlendStartSamples = processor.lastRenderedOutputSamples;
    processor.sceneRecallBlendTotalSamples = blendSamples;
    processor.sceneRecallBlendSamplesRemaining = blendSamples;

    syncSceneModeFromParameters(processor);
    if (processor.isSceneModeEnabled())
        clearAllStripGroupsForSceneMode(processor);
}

void SceneScheduler::performSceneLoad(MlrVSTAudioProcessor& processor,
                                      int mainPresetIndex,
                                      int sceneSlot,
                                      double hostPpqSnapshot,
                                      double hostTempoSnapshot,
                                      int64_t hostGlobalSampleSnapshot)
{
    if (!processor.audioEngine)
        return;

    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    if (!PresetStore::presetExists(storageIndex))
        return;

    ScopedSuspendProcessing scopedSuspend(processor);

    const auto preservedScenePadHeld = processor.scenePadHeld;
    const auto preservedScenePadHoldDeleteTriggered = processor.scenePadHoldDeleteTriggered;
    const auto preservedScenePadPressStartMs = processor.scenePadPressStartMs;
    const auto preservedScenePadActionBurstUntilMs = processor.scenePadActionBurstUntilMs;
    const auto preservedScenePadLastTapMs = processor.scenePadLastTapMs;
    const int preservedSceneCopySourceSlot = processor.sceneCopySourceSlot;
    const int preservedSceneCopyMainPresetIndex = processor.sceneCopyMainPresetIndex;

    processor.resetRuntimePresetStateToDefaults();
    processor.scenePadHeld = preservedScenePadHeld;
    processor.scenePadHoldDeleteTriggered = preservedScenePadHoldDeleteTriggered;
    processor.scenePadPressStartMs = preservedScenePadPressStartMs;
    processor.scenePadActionBurstUntilMs = preservedScenePadActionBurstUntilMs;
    processor.scenePadLastTapMs = preservedScenePadLastTapMs;
    processor.sceneCopySourceSlot = preservedSceneCopySourceSlot;
    processor.sceneCopyMainPresetIndex = preservedSceneCopyMainPresetIndex;
    for (auto& f : processor.currentStripFiles)
        f = juce::File();

    const bool loadSucceeded = PresetStore::loadPreset(
        storageIndex,
        MlrVSTAudioProcessor::MaxStrips,
        processor.audioEngine.get(),
        processor.parameters,
        [&processor](int stripIndex, const juce::File& sampleFile)
        {
            return processor.loadSampleToStrip(stripIndex, sampleFile);
        },
        [&processor](int stripIndex, const juce::File& sampleFile)
        {
            processor.rememberLoadedSamplePathForStrip(stripIndex, sampleFile, false);
        },
        [&processor](int stripIndex,
                     const juce::File& loopDir,
                     const juce::File& stepDir,
                     const juce::File& flipDir)
        {
            processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Loop, loopDir, false);
            processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Step, stepDir, false);
            processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Flip, flipDir, false);
        },
        [&processor](int stripIndex, const juce::XmlElement* flipStateXml)
        {
            processor.applyFlipPresetStateXml(stripIndex, flipStateXml);
        },
        [&processor](int stripIndex, const juce::XmlElement* loopPitchStateXml)
        {
            processor.applyLoopPitchPresetStateXml(stripIndex, loopPitchStateXml);
        },
        [&processor, sceneSlot](const juce::XmlElement& presetXml)
        {
            applySceneChainStateXml(processor, presetXml.getChildByName("SceneChainState"), sceneSlot);
        },
        hostPpqSnapshot,
        hostTempoSnapshot,
        true,
        hostGlobalSampleSnapshot);

    if (loadSucceeded)
    {
        processor.normalizeLoopPitchMasterRoles();
        processor.applyLoopPitchSyncToAllStrips();

        if (std::isfinite(hostPpqSnapshot) && std::isfinite(hostTempoSnapshot) && hostTempoSnapshot > 0.0)
        {
            const int64_t recallSample = hostGlobalSampleSnapshot >= 0
                ? hostGlobalSampleSnapshot
                : processor.audioEngine->getGlobalSampleCount();

            for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            {
                auto* strip = processor.audioEngine->getStrip(stripIndex);
                if (strip == nullptr || !strip->isPlaying())
                    continue;

                if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                    continue;

                const bool hasStripAudio = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                    ? processor.hasSampleModeAudio(stripIndex)
                    : strip->hasAudio();
                if (!hasStripAudio)
                    continue;

                const float manualBeats = strip->getBeatsPerLoop();
                const double beatsForLoop = (manualBeats >= 0.0f)
                    ? static_cast<double>(manualBeats)
                    : static_cast<double>(juce::jmax(1, strip->getRecordingBars()) * 4);
                if (!std::isfinite(beatsForLoop) || beatsForLoop <= 0.0)
                    continue;

                double alignedOffsetBeats = std::fmod(-hostPpqSnapshot, beatsForLoop);
                if (alignedOffsetBeats < 0.0)
                    alignedOffsetBeats += beatsForLoop;

                // Scene recalls should give every active loop a fresh host-aligned
                // timeline anchor so Pattern End measures the newly loaded scene,
                // not any stale/free-running phase left over from the previous one.
                strip->restorePresetPpqState(true,
                                             true,
                                             alignedOffsetBeats,
                                             strip->getLoopStart(),
                                             hostTempoSnapshot,
                                             hostPpqSnapshot,
                                             recallSample);
            }
        }

        const int blendSamples = juce::jmax(12, static_cast<int>(std::round(processor.currentSampleRate * 0.00035)));
        processor.sceneRecallBlendStartSamples = processor.lastRenderedOutputSamples;
        processor.sceneRecallBlendTotalSamples = blendSamples;
        processor.sceneRecallBlendSamplesRemaining = blendSamples;
    }

    syncSceneModeFromParameters(processor);
    if (processor.isSceneModeEnabled())
        clearAllStripGroupsForSceneMode(processor);
}

void SceneScheduler::appendSceneModeStateToState(const MlrVSTAudioProcessor& processor, juce::ValueTree& state)
{
    auto sceneState = state.getOrCreateChildWithName("SceneModeState", nullptr);
    sceneState.setProperty("activeMainPresetIndex", processor.activeSceneMainPresetIndex, nullptr);
    sceneState.setProperty("activeSceneSlot", processor.activeSceneSlot, nullptr);
    sceneState.setProperty("groupSnapshotValid", processor.sceneModeGroupSnapshot.valid, nullptr);

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        sceneState.setProperty("stripGroup" + juce::String(stripIndex),
                               processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)],
                               nullptr);
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        sceneState.setProperty("groupVolume" + juce::String(groupIndex),
                               processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)],
                               nullptr);
        sceneState.setProperty("groupMuted" + juce::String(groupIndex),
                               processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)],
                               nullptr);
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        sceneState.setProperty("sceneRepeat" + juce::String(sceneSlot),
                               getSceneRepeatCount(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneLengthMode" + juce::String(sceneSlot),
                               getSceneLengthModeIndex(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneManualBars" + juce::String(sceneSlot),
                               getSceneManualBars(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneAnchorStrip" + juce::String(sceneSlot),
                               getSceneAnchorStrip(processor, sceneSlot),
                               nullptr);
    }
}

void SceneScheduler::loadSceneModeStateFromState(MlrVSTAudioProcessor& processor, const juce::ValueTree& state)
{
    processor.sceneModeGroupSnapshot = {};
    processor.sceneModeGroupSnapshot.stripGroups.fill(-1);
    processor.sceneModeGroupSnapshot.groupVolumes.fill(1.0f);
    processor.sceneModeGroupSnapshot.groupMuted.fill(false);
    processor.sceneRepeatCounts.fill(1);
    processor.sceneLengthModes.fill(static_cast<int>(SceneLengthMode::LongestStrip));
    processor.sceneManualBars.fill(4);
    processor.sceneAnchorStrips.fill(0);
    processor.activeSceneMainPresetIndex = 0;
    processor.activeSceneSlot = 0;

    auto sceneState = state.getChildWithName("SceneModeState");
    if (!sceneState.isValid())
        return;

    processor.activeSceneMainPresetIndex = juce::jlimit(
        0, MlrVSTAudioProcessor::MaxPresetSlots - 1, static_cast<int>(sceneState.getProperty("activeMainPresetIndex", 0)));
    processor.activeSceneSlot = juce::jlimit(
        0, MlrVSTAudioProcessor::SceneSlots - 1, static_cast<int>(sceneState.getProperty("activeSceneSlot", 0)));
    processor.sceneModeGroupSnapshot.valid = static_cast<bool>(sceneState.getProperty("groupSnapshotValid", false));

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)] = static_cast<int>(
            sceneState.getProperty("stripGroup" + juce::String(stripIndex), -1));
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)] = static_cast<float>(
            sceneState.getProperty("groupVolume" + juce::String(groupIndex), 1.0f));
        processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)] = static_cast<bool>(
            sceneState.getProperty("groupMuted" + juce::String(groupIndex), false));
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        setSceneRepeatCount(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneRepeat" + juce::String(sceneSlot), 1)));
        setSceneLengthModeIndex(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty(
                "sceneLengthMode" + juce::String(sceneSlot),
                static_cast<int>(SceneLengthMode::LongestStrip))));
        setSceneManualBars(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneManualBars" + juce::String(sceneSlot), 4)));
        setSceneAnchorStrip(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneAnchorStrip" + juce::String(sceneSlot), 0)));
    }
}
