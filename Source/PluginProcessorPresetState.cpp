/*
  ==============================================================================

    PluginProcessorPresetState.cpp
    Preset and state persistence implementation split from PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "GlobalSettingsStore.h"
#include "PresetStore.h"
#include "SceneScheduler.h"

#include <limits>
#include <utility>

namespace
{
constexpr const char* kEmbeddedFlipSampleAttr = "embeddedSampleWavBase64";

bool pathUsesMissingVolumeMount(const juce::String& rawPath)
{
    static constexpr const char* kVolumesPrefix = "/Volumes/";
    auto path = rawPath.trim();
    if (!path.startsWith(kVolumesPrefix))
        return false;

    path = path.fromFirstOccurrenceOf(kVolumesPrefix, false, false);
    const int slashIndex = path.indexOfChar('/');
    const auto volumeName = (slashIndex >= 0 ? path.substring(0, slashIndex) : path).trim();
    if (volumeName.isEmpty())
        return false;

    return !juce::File("/Volumes").getChildFile(volumeName).exists();
}

bool canSafelyProbeFilesystemPath(const juce::File& file)
{
    if (file == juce::File())
        return false;

    const auto path = file.getFullPathName().trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return false;

    return !pathUsesMissingVolumeMount(path);
}

bool safeFileExistsAsFile(const juce::File& file)
{
    return canSafelyProbeFilesystemPath(file) && file.existsAsFile();
}

void appendSceneDebugLog(const juce::String& message)
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("mlrvst_scene_debug.log");
    juce::FileOutputStream out(logFile);
    if (!out.openedOk())
        return;

    out.setPosition(logFile.existsAsFile() ? logFile.getSize() : 0);
    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S.%3Q");
    out.writeText(timestamp + " " + message + "\n", false, false, "\n");
    out.flush();
}

juce::String controlModeToKey(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Speed: return "speed";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "pitch";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "pan";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "volume";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "grainsize";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "filter";
        case MlrVSTAudioProcessor::ControlMode::Delay: return "delay";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "swing";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "gate";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "browser";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "group";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "modulation";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "preset";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "stepedit";
        case MlrVSTAudioProcessor::ControlMode::Normal:
        default: return "normal";
    }
}

bool controlModeFromKey(const juce::String& key, MlrVSTAudioProcessor::ControlMode& mode)
{
    const auto normalized = key.trim().toLowerCase();
    if (normalized == "speed") { mode = MlrVSTAudioProcessor::ControlMode::Speed; return true; }
    if (normalized == "pitch") { mode = MlrVSTAudioProcessor::ControlMode::Pitch; return true; }
    if (normalized == "pan") { mode = MlrVSTAudioProcessor::ControlMode::Pan; return true; }
    if (normalized == "volume") { mode = MlrVSTAudioProcessor::ControlMode::Volume; return true; }
    if (normalized == "grainsize" || normalized == "grain_size" || normalized == "grain") { mode = MlrVSTAudioProcessor::ControlMode::GrainSize; return true; }
    if (normalized == "filter") { mode = MlrVSTAudioProcessor::ControlMode::Filter; return true; }
    if (normalized == "delay") { mode = MlrVSTAudioProcessor::ControlMode::Delay; return true; }
    if (normalized == "swing") { mode = MlrVSTAudioProcessor::ControlMode::Swing; return true; }
    if (normalized == "gate") { mode = MlrVSTAudioProcessor::ControlMode::Gate; return true; }
    if (normalized == "browser") { mode = MlrVSTAudioProcessor::ControlMode::FileBrowser; return true; }
    if (normalized == "group") { mode = MlrVSTAudioProcessor::ControlMode::GroupAssign; return true; }
    if (normalized == "mod" || normalized == "modulation") { mode = MlrVSTAudioProcessor::ControlMode::Modulation; return true; }
    if (normalized == "preset") { mode = MlrVSTAudioProcessor::ControlMode::Preset; return true; }
    if (normalized == "stepedit" || normalized == "step_edit" || normalized == "step") { mode = MlrVSTAudioProcessor::ControlMode::StepEdit; return true; }
    return false;
}

struct ScopedSceneAutosaveSuppression
{
    explicit ScopedSceneAutosaveSuppression(MlrVSTAudioProcessor& p) : processor(p)
    {
        processor.beginSceneAutosaveSuppression();
    }

    ~ScopedSceneAutosaveSuppression()
    {
        processor.endSceneAutosaveSuppression();
    }

    MlrVSTAudioProcessor& processor;
};

struct ScopedSuspendProcessing
{
    explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p)
    {
        processor.suspendProcessing(true);
    }

    ~ScopedSuspendProcessing()
    {
        processor.suspendProcessing(false);
    }

    MlrVSTAudioProcessor& processor;
};
} // namespace

class MlrVSTAudioProcessor::PresetSaveJob final : public juce::ThreadPoolJob
{
public:
    PresetSaveJob(MlrVSTAudioProcessor& ownerIn, PresetSaveRequest requestIn)
        : juce::ThreadPoolJob("mlrVSTPresetSave_" + juce::String(requestIn.presetIndex + 1)),
          owner(ownerIn),
          request(std::move(requestIn))
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit())
        {
            owner.pushPresetSaveResult({ request.presetIndex, false });
            return jobHasFinished;
        }

        const bool success = owner.runPresetSaveRequest(request);
        owner.pushPresetSaveResult({ request.presetIndex, success });
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    PresetSaveRequest request;
};

void MlrVSTAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    try
    {
        if (isSceneModeEnabled() && audioEngine != nullptr)
            syncSceneMotionStateFromEngine(activeSceneSlot);

        auto state = parameters.copyState();
        stripPersistentGlobalControlsFromState(state);
        appendDefaultPathsToState(state);
        appendControlPagesToState(state);
        appendFlipStatesToState(state);
        appendLoopPitchStateToState(state);

        if (!state.isValid())
            return;

        std::unique_ptr<juce::XmlElement> xml(state.createXml());

        if (xml != nullptr)
        {
            copyXmlToBinary(*xml, destData);
        }
    }
    catch (...)
    {
        // If anything goes wrong, just return empty state
        destData.reset();
    }
}

void MlrVSTAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    resetRuntimePresetStateToDefaults();
    loadedPresetIndex = -1;

    if (xmlState != nullptr)
        if (xmlState->hasTagName(parameters.state.getType()))
        {
            suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
            auto state = juce::ValueTree::fromXml(*xmlState);
            stripPersistentGlobalControlsFromState(state);
            parameters.replaceState(state);
            loadDefaultPathsFromState(state);
            loadControlPagesFromState(state);
            loadFlipStatesFromState(state);
            loadLoopPitchStateFromState(state);
            GlobalSettingsStore::loadGlobalControls(*this);
            persistentGlobalControlsApplied = true;
            pendingPersistentGlobalControlsRestore.store(1, std::memory_order_release);
            pendingPersistentGlobalControlsRestoreMs = juce::Time::currentTimeMillis() + 250;
            pendingPersistentGlobalControlsRestoreRemaining = 5;
            suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
            persistentGlobalControlsReady.store(1, std::memory_order_release);
            syncSceneModeFromParameters();
            lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
            lastScenePerformanceProcessSceneSlot = -1;
            lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
            if (isSceneModeEnabled())
                clearAllStripGroupsForSceneMode();
        }
}


void MlrVSTAudioProcessor::appendControlPagesToState(juce::ValueTree& state) const
{
    auto controlPages = state.getOrCreateChildWithName("ControlPages", nullptr);
    const auto orderSnapshot = getControlPageOrder();
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]), nullptr);
    }

    controlPages.setProperty("momentary", isControlPageMomentary(), nullptr);
    controlPages.setProperty("swingDivision", swingDivisionSelection.load(std::memory_order_acquire), nullptr);
}

void MlrVSTAudioProcessor::appendFlipStatesToState(juce::ValueTree& state) const
{
    auto flipStates = state.getOrCreateChildWithName("FlipStates", nullptr);
    while (flipStates.getNumChildren() > 0)
        flipStates.removeChild(0, nullptr);

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine != nullptr ? audioEngine->getStrip(i) : nullptr;
        if (strip == nullptr)
            continue;

        const bool isFlipMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);
        if (!isFlipMode && !hasSampleModeAudio(i))
            continue;

        auto* engine = const_cast<MlrVSTAudioProcessor*>(this)->getSampleModeEngine(i, isFlipMode);
        if (engine == nullptr)
            continue;

        auto flipState = engine->capturePersistentState().createValueTree("FlipStrip");
        flipState.setProperty("index", i, nullptr);
        flipState.setProperty("enabled", isFlipMode, nullptr);
        const auto embeddedSample = createEmbeddedFlipSampleData(i);
        if (embeddedSample.isNotEmpty())
            flipState.setProperty(kEmbeddedFlipSampleAttr, embeddedSample, nullptr);
        flipStates.addChild(flipState, -1, nullptr);
    }
}

void MlrVSTAudioProcessor::appendSceneModeStateToState(juce::ValueTree& state) const
{
    SceneScheduler::appendSceneModeStateToState(*this, state);
}

void MlrVSTAudioProcessor::loadSceneModeStateFromState(const juce::ValueTree& state)
{
    SceneScheduler::loadSceneModeStateFromState(*this, state);
    reloadAllSceneChainTransitionEndSamples();
}

void MlrVSTAudioProcessor::loadFlipStatesFromState(const juce::ValueTree& state)
{
    auto flipStates = state.getChildWithName("FlipStates");
    if (!flipStates.isValid() || audioEngine == nullptr)
        return;

    for (auto flipState : flipStates)
    {
        if (!flipState.hasType("FlipStrip"))
            continue;

        const int stripIndex = static_cast<int>(flipState.getProperty("index", -1));
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            continue;

        if (auto* strip = audioEngine->getStrip(stripIndex))
        {
            const bool enabled = static_cast<bool>(flipState.getProperty("enabled", false));
            if (!enabled)
                continue;

            strip->setPlayMode(EnhancedAudioStrip::PlayMode::Sample);
            if (auto* engine = getSampleModeEngine(stripIndex, true))
            {
                const auto persistentState = SampleModePersistentState::fromValueTree(flipState);
                const auto embeddedSample = flipState.getProperty(kEmbeddedFlipSampleAttr).toString();
                const juce::File sampleFile(persistentState.samplePath);
                if (persistentState.samplePath.isNotEmpty())
                    rememberLoadedSamplePathForStripMode(stripIndex, sampleFile, SamplePathMode::Flip, false);
                if (safeFileExistsAsFile(sampleFile))
                {
                    engine->applyPersistentState(persistentState);
                    loadSampleToSampleModeStrip(stripIndex, sampleFile);
                }
                else if (embeddedSample.isNotEmpty())
                {
                    loadEmbeddedFlipSampleData(stripIndex, embeddedSample, &persistentState);
                }
                else
                {
                    engine->applyPersistentState(persistentState);
                }
            }
        }
    }
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createFlipPresetStateXml(int stripIndex) const
{
    auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
    if (strip == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return {};

    auto* engine = const_cast<MlrVSTAudioProcessor*>(this)->getSampleModeEngine(stripIndex, false);
    if (engine == nullptr)
        return {};

    auto xml = engine->capturePersistentState().createXml("FlipState");
    const auto embeddedSample = createEmbeddedFlipSampleData(stripIndex);
    if (embeddedSample.isNotEmpty())
        xml->setAttribute(kEmbeddedFlipSampleAttr, embeddedSample);
    return xml;
}

void MlrVSTAudioProcessor::applyFlipPresetStateXml(int stripIndex, const juce::XmlElement* flipStateXml)
{
    if (flipStateXml == nullptr || audioEngine == nullptr || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    strip->setPlayMode(EnhancedAudioStrip::PlayMode::Sample);
    if (auto* engine = getSampleModeEngine(stripIndex, true))
    {
        const auto persistentState = SampleModePersistentState::fromXml(*flipStateXml);
        const juce::File sampleFile(persistentState.samplePath);
        if (persistentState.samplePath.isNotEmpty())
            rememberLoadedSamplePathForStripMode(stripIndex, sampleFile, SamplePathMode::Flip, false);
        const auto embeddedSample = flipStateXml->getStringAttribute(kEmbeddedFlipSampleAttr);
        bool loaded = false;
        if (embeddedSample.isNotEmpty())
        {
            loaded = loadEmbeddedFlipSampleData(stripIndex, embeddedSample, &persistentState);
        }
        if (!loaded && safeFileExistsAsFile(sampleFile))
        {
            loaded = loadSampleFileIntoSampleModeEngine(*engine, sampleFile);
            if (loaded)
            {
                engine->applyPersistentState(persistentState);
                handleSampleModeLegacyLoopRenderStateChanged(stripIndex);
            }
        }
        if (!loaded)
        {
            if (persistentState.samplePath.isNotEmpty())
            {
                engine->clear();
                currentStripFiles[static_cast<size_t>(stripIndex)] = juce::File();
                return;
            }
            engine->applyPersistentState(persistentState);
        }
    }
}

void MlrVSTAudioProcessor::loadControlPagesFromState(const juce::ValueTree& state)
{
    auto controlPages = state.getChildWithName("ControlPages");
    if (!controlPages.isValid())
    {
        GlobalSettingsStore::saveControlPages(*this);
        return;
    }

    ControlPageOrder parsedOrder{};
    int parsedCount = 0;

    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        const auto value = controlPages.getProperty(key).toString();
        ControlMode mode = ControlMode::Normal;
        if (!controlModeFromKey(value, mode) || mode == ControlMode::Normal)
            continue;

        bool duplicate = false;
        for (int j = 0; j < parsedCount; ++j)
        {
            if (parsedOrder[static_cast<size_t>(j)] == mode)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        parsedOrder[static_cast<size_t>(parsedCount)] = mode;
        ++parsedCount;
    }

    const ControlPageOrder defaultOrder {
        ControlMode::Speed,
        ControlMode::Pan,
        ControlMode::Volume,
        ControlMode::GrainSize,
        ControlMode::Swing,
        ControlMode::Gate,
        ControlMode::FileBrowser,
        ControlMode::GroupAssign,
        ControlMode::Filter,
        ControlMode::Delay,
        ControlMode::Pitch,
        ControlMode::Modulation,
        ControlMode::Preset,
        ControlMode::StepEdit
    };

    for (const auto mode : defaultOrder)
    {
        bool alreadyPresent = false;
        for (int i = 0; i < parsedCount; ++i)
        {
            if (parsedOrder[static_cast<size_t>(i)] == mode)
            {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent && parsedCount < NumControlRowPages)
            parsedOrder[static_cast<size_t>(parsedCount++)] = mode;
    }

    if (parsedCount == NumControlRowPages)
    {
        const juce::ScopedLock lock(controlPageOrderLock);
        controlPageOrder = parsedOrder;
    }

    const bool momentary = controlPages.getProperty("momentary", true);
    controlPageMomentary.store(momentary, std::memory_order_release);
    const int swingDivision = static_cast<int>(controlPages.getProperty("swingDivision", 1));
    setSwingDivisionSelection(swingDivision);
    GlobalSettingsStore::saveControlPages(*this);
}

void MlrVSTAudioProcessor::resetRuntimePresetStateToDefaults(bool preserveLoadedStripAudio)
{
    if (!audioEngine)
        return;

    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    clearPendingActiveSceneAutosave();

    pendingPresetLoadIndex.store(-1, std::memory_order_release);
    clearPendingSceneApplyState();
    pendingScenePreloadDirty.store(0, std::memory_order_release);
    pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None), std::memory_order_release);
    pendingSceneRecall = {};
    requestAbortActiveSceneTransition();
    suppressOwnedStripParameterSync.store(0, std::memory_order_release);
    std::unique_ptr<juce::ValueTree> pendingState(
        pendingSceneParameterStatePtr.exchange(nullptr, std::memory_order_acq_rel));
    for (auto& slot : retiredPendingSceneParameterStates)
    {
        std::unique_ptr<juce::ValueTree> retiredState(
            slot.exchange(nullptr, std::memory_order_acq_rel));
    }
    switchScenePlaybackOwner(ScenePlaybackOwner::Manual, false);
    sceneChainState = {};
    activeSceneMainPresetIndex = 0;
    activeSceneSlot = 0;
    focusedSceneSlot = 0;
    activeSceneNeedsCaptureBeforeManualRecall = true;
    clearActiveScenePlaybackHandle();
    scenePadHeld.fill(false);
    scenePadHoldDeleteTriggered.fill(false);
    scenePadLaunchConsumed.fill(false);
    scenePadPressStartMs.fill(0);
    scenePadActionBurstUntilMs.fill(0);
    scenePadLastTapMs.fill(0);
    monomePatternPadPendingUntilMs.fill(0);
    monomePatternPadPendingAction.fill(MonomePatternTapAction::None);
    clearPendingMonomeSceneRecorderTap();
    clearPendingSceneRecorderAction();
    pendingSceneTriggerRecords.fill({});
    scenePerformanceRecorder.clearAll();
    lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    lastScenePerformanceProcessSceneSlot = -1;
    lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    scenePlaybackBlockStutterPostRenderPending = false;
    scenePlaybackBlockStutterPostRenderAmount = 0.0f;
    sceneCopySourceSlot = -1;
    sceneCopyMainPresetIndex = 0;
    scenePerformanceClipboardData.reset();
    sceneMainAutomationDisplayTargets.fill(ModernAudioEngine::ModTarget::None);
    macroTargetAssignments[5].store(static_cast<int>(getDefaultMacroTarget(5)), std::memory_order_release);
    macroTargetAssignments[6].store(static_cast<int>(getDefaultMacroTarget(6)), std::memory_order_release);
    macroTargetAssignments[7].store(static_cast<int>(getDefaultMacroTarget(7)), std::memory_order_release);

    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (auto& pending : pendingLoopChanges)
            pending = PendingLoopChange{};
    }
    {
        const juce::ScopedLock lock(pendingBarChangeLock);
        for (auto& pending : pendingBarChanges)
            pending = PendingBarChange{};
    }
    pendingBarLengthApply.fill(false);
    momentaryScratchHoldActive = false;
    momentaryStutterHoldActive = false;
    momentaryStutterActiveDivisionButton = -1;
    momentaryStutterButtonMask.store(0, std::memory_order_release);
    momentaryStutterMacroBaselineCaptured = false;
    momentaryStutterMacroCapturePending = false;
    momentaryStutterMacroStartPpq = 0.0;
    momentaryStutterRecordedDivisionButton = -1;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
    momentaryStutterStripArmed.fill(false);
    momentaryStutterPlaybackActive.store(0, std::memory_order_release);
    sceneGlobalStutterBaseAmount.store(0.0f, std::memory_order_release);
    clearSceneBoundaryTransitionState();
    clearSceneChainReturnOverride();
    lastSceneMotionSyncTimeMs = 0;
    lastPitchCacheRefreshTimeMs = 0;
    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartDivisionBeats.store(1.0, std::memory_order_release);
    pendingStutterStartQuantizeDivision.store(8, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
    for (auto& saved : momentaryStutterSavedState)
        saved = MomentaryStutterSavedStripState{};
    pendingStutterReleaseActive.store(0, std::memory_order_release);
    pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
    audioEngine->setMomentaryStutterActive(false);
    audioEngine->setMomentaryStutterStartPpq(-1.0);
    audioEngine->setMomentaryStutterReleasePpq(-1.0);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->clearMomentaryStutterStrips();
    audioEngine->clearRecentInputBuffer();
    for (auto& inFlight : loopStripLoadInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopStripLoadRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopStripLoadProgressPermille)
        progress.store(0, std::memory_order_release);
    for (auto& inFlight : loopPitchAnalysisInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopPitchAnalysisRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopPitchAnalysisProgressPermille)
        progress.store(0, std::memory_order_release);
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
    for (auto& role : loopPitchRoles)
        role.store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    for (auto& timing : loopPitchSyncTimings)
        timing.store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    for (auto& assignedMidi : loopPitchAssignedMidi)
        assignedMidi.store(-1, std::memory_order_release);
    for (auto& assignedManual : loopPitchAssignedManual)
        assignedManual.store(0, std::memory_order_release);
    for (auto& pendingRetune : loopPitchPendingRetune)
        pendingRetune.store(0, std::memory_order_release);
    loopStripLoadStatusTexts.fill({});
    loopPitchAnalysisStatusTexts.fill({});
    loopPitchLastObservedColumns.fill(-1);
    loopPitchLastObservedHostBar = -1;
    globalRootNoteMidi.store(60, std::memory_order_release);
    globalPitchScale.store(static_cast<int>(ModernAudioEngine::PitchScale::Chromatic), std::memory_order_release);

    for (int i = 0; i < MaxStrips; ++i)
    {
        pendingLoopStripFiles[static_cast<size_t>(i)] = juce::File();
        if (!preserveLoadedStripAudio)
            currentStripFiles[static_cast<size_t>(i)] = juce::File();

        if (auto* sampleEngine = getSampleModeEngine(i, false))
        {
            sampleEngine->clearPendingVisibleSlice();
            sampleEngine->stop();
            sampleEngine->clear();
        }
        invalidateFlipLegacyLoopSync(i);

        if (auto* strip = audioEngine->getStrip(i))
        {
            if (!preserveLoadedStripAudio)
            {
                strip->clearSample();
                strip->stop(true);
                strip->setLoop(0, MaxColumns);
                strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
                strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
                strip->setReverse(false);
                setStripVolumeControlValue(i, 1.0f, StripControlWriteMode::CacheOnly);
                strip->setTrimDb(0.0f);
                setStripPanControlValue(i, 0.0f, StripControlWriteMode::CacheOnly);
                setStripSpeedControlValue(i, 1.0f, StripControlWriteMode::CacheOnly);
                strip->setBeatsPerLoop(-1.0f);
                strip->setScratchAmount(0.0f);
                strip->setTransientSliceMode(false);
                strip->setLoopSliceLength(1.0f);
                strip->setResamplePitchEnabled(false);
                strip->setResamplePitchRatio(1.0f);
                strip->setPitchShift(0.0f);
                strip->setRecordingBars(2);
                setStripFilterFrequencyControlValue(i, 20000.0f, StripControlWriteMode::CacheOnly);
                setStripFilterResonanceControlValue(i, 0.707f, StripControlWriteMode::CacheOnly);
                setStripFilterMorphControlValue(i, 0.0f, StripControlWriteMode::CacheOnly);
                setStripFilterAlgorithmControlValue(i, EnhancedAudioStrip::FilterAlgorithm::Tpt12, StripControlWriteMode::CacheOnly);
                setStripFilterEnabledControlValue(i, false, StripControlWriteMode::CacheOnly);
                strip->setSwingAmount(0.0f);
                strip->setGateAmount(0.0f);
                strip->setGateSpeed(4.0f);
                strip->setGateEnvelope(0.5f);
                strip->setGateShape(0.5f);
                strip->setStepPatternBars(1);
                strip->setStepPage(0);
                strip->currentStep = 0;
                strip->stepPattern.fill(false);
                strip->stepSubdivisionStartVelocity.fill(1.0f);
                strip->stepSubdivisions.fill(1);
                strip->stepSubdivisionRepeatVelocity.fill(1.0f);
                strip->stepProbability.fill(1.0f);
                strip->setStepEnvelopeAttackMs(0.0f);
                strip->setStepEnvelopeDecayMs(4000.0f);
                strip->setStepEnvelopeReleaseMs(110.0f);
                strip->setGrainSizeMs(1240.0f);
                strip->setGrainDensity(0.05f);
                strip->setGrainPitch(0.0f);
                strip->setGrainPitchJitter(0.0f);
                strip->setGrainSpread(0.0f);
                strip->setGrainJitter(0.0f);
                strip->setGrainPositionJitter(0.0f);
                strip->setGrainRandomDepth(0.0f);
                strip->setGrainArpDepth(0.0f);
                strip->setGrainCloudDepth(0.0f);
                strip->setGrainEmitterDepth(0.0f);
                strip->setGrainEnvelope(0.0f);
                strip->setGrainShape(0.0f);
                strip->setGrainArpMode(0);
                strip->setGrainTempoSyncEnabled(true);
            }
        }

        audioEngine->assignStripToGroup(i, -1);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            audioEngine->resetModSequencerSlotToDefaults(i, slot);
        }
        audioEngine->setModSequencerSlot(i, 0);

        if (!preserveLoadedStripAudio)
        {
            if (auto* param = parameters.getParameter("stripVolume" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripTrimDb" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripPan" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripSpeed" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripPitch" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripSliceLength" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripPitchControlMode" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripTempoMatchMode" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripFilterEnabled" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripFilterFrequency" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripFilterResonance" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripFilterMorph" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripFilterAlgorithm" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayMix" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayTime" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayFeedback" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayLowCut" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayHighCut" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
            if (auto* param = parameters.getParameter("stripDelayMode" + juce::String(i)))
                param->setValueNotifyingHost(param->getDefaultValue());
        }
    }

    for (int i = 0; i < ModernAudioEngine::MaxGroups; ++i)
    {
        if (auto* group = audioEngine->getGroup(i))
        {
            group->setVolume(1.0f);
            group->setMuted(false);
        }
    }

    for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
        audioEngine->clearPattern(i);
}

void MlrVSTAudioProcessor::initRuntimeStateToDefaults()
{
    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    for (auto* parameter : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
            ranged->setValueNotifyingHost(ranged->getDefaultValue());
    }

    resetRuntimePresetStateToDefaults();
    sceneRepeatCounts.fill(1);
    sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    sceneManualBars.fill(4);
    sceneAnchorStrips.fill(0);
    clearAllSceneClipSlotRuntimeStates();
    sceneModeGroupSnapshot.valid = false;
    resetCurrentBrowserDirectoriesToDefaultPaths(true);
    loadedPresetIndex = -1;
    juce::ignoreUnused(ensureSceneSlotFallbackState(getActiveMainPresetIndexForScenes(), getActiveSceneSlot()));
    updateMonomeLEDs();
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::isHostTransportPlaying() const
{
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo))
        return positionInfo.getIsPlaying();

    return lastHostTransportPlaying.load(std::memory_order_acquire) != 0;
}

bool MlrVSTAudioProcessor::getCurrentHostPositionInfo(juce::AudioPlayHead::PositionInfo& outPosition) const
{
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            outPosition = *position;
            return true;
        }
    }

    return false;
}

bool MlrVSTAudioProcessor::getCurrentHostPpq(double& outPpq) const
{
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo) && positionInfo.getPpqPosition().hasValue())
    {
        outPpq = *positionInfo.getPpqPosition();
        return true;
    }

    return false;
}

bool MlrVSTAudioProcessor::getHostSyncSnapshot(double& outPpq, double& outTempo) const
{
    const bool hostTransportPlaying = isHostTransportPlaying();
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo))
    {
        if (positionInfo.getIsPlaying()
            && positionInfo.getPpqPosition().hasValue()
            && positionInfo.getBpm().hasValue()
            && std::isfinite(*positionInfo.getPpqPosition())
            && std::isfinite(*positionInfo.getBpm())
            && *positionInfo.getBpm() > 0.0)
        {
            outPpq = *positionInfo.getPpqPosition();
            outTempo = *positionInfo.getBpm();
            return true;
        }
    }

    if (audioEngine != nullptr)
    {
        const double fallbackPpq = audioEngine->getTimelineBeat();
        const double fallbackTempo = audioEngine->getCurrentTempo();
        if (hostTransportPlaying
            && std::isfinite(fallbackPpq)
            && std::isfinite(fallbackTempo)
            && fallbackTempo > 0.0)
        {
            outPpq = fallbackPpq;
            outTempo = fallbackTempo;
            return true;
        }
    }

    return false;
}

void MlrVSTAudioProcessor::refreshUtilityTimerCadence()
{
    SceneSwitchEvent pendingSwitchEvent;
    const bool queuedSceneApplyActive = peekPendingSceneApplyState(pendingSwitchEvent);
    const bool queuedSequenceDriven = queuedSceneApplyActive && pendingSwitchEvent.sequenceDriven;
    const bool transitionWindowActive = queuedSceneApplyActive
        && (!queuedSequenceDriven || isHostTransportPlaying());
    bool pendingSceneRecallNearBoundary = false;
    if (pendingSceneRecall.active && !transitionWindowActive)
    {
        double currentPpq = std::numeric_limits<double>::quiet_NaN();
        double currentTempo = std::numeric_limits<double>::quiet_NaN();
        bool hasTimingReference = getHostSyncSnapshot(currentPpq, currentTempo);
        if (!hasTimingReference && audioEngine != nullptr)
        {
            const double fallbackPpq = audioEngine->getTimelineBeat();
            const double fallbackTempo = juce::jmax(1.0, audioEngine->getCurrentTempo());
            if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
            {
                currentPpq = fallbackPpq;
                currentTempo = fallbackTempo;
                hasTimingReference = true;
            }
        }

        if (hasTimingReference
            && pendingSceneRecall.targetResolved
            && std::isfinite(pendingSceneRecall.targetPpq)
            && std::isfinite(currentPpq)
            && std::isfinite(currentTempo)
            && currentTempo > 0.0)
        {
            const double beatsUntilTarget = pendingSceneRecall.targetPpq - currentPpq;
            const double lookaheadBeats = juce::jlimit(0.25,
                                                       8.0,
                                                       (1.25 * currentTempo) / 60.0);
            pendingSceneRecallNearBoundary = beatsUntilTarget <= lookaheadBeats;
        }
    }

    const bool needsScenePolling = pendingSceneRecallNearBoundary
        || pendingScenePreloadDirty.load(std::memory_order_acquire) != 0
        || pendingSceneRecorderAction.active
        || pendingSceneRecorderApplyAction.load(std::memory_order_acquire) != static_cast<int>(SceneRecorderAction::None)
        || transitionWindowActive;
    const int desiredIntervalMs = transitionWindowActive
        ? kSceneRecallFastRefreshMs
        : (needsScenePolling
            ? kSceneRecallArmedRefreshMs
            : (monomeConnection.supportsArc() ? kArcRefreshMs : kGridRefreshMs));

    if (!isTimerRunning() || getTimerInterval() != desiredIntervalMs)
        startTimer(desiredIntervalMs);
}

void MlrVSTAudioProcessor::performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot)
{
    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    // Always reset to a known clean runtime state before applying preset data.
    // This guarantees no strip audio/params leak across preset transitions.
    resetRuntimePresetStateToDefaults();
    sceneRepeatCounts.fill(1);
    sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    sceneManualBars.fill(4);
    sceneAnchorStrips.fill(0);
    clearAllSceneClipSlotRuntimeStates();
    clearStoredSceneSlotStates(presetIndex);
    loadedPresetIndex = -1;
    activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);
    activeSceneSlot = 0;
    focusedSceneSlot = 0;
    focusSceneSlot(activeSceneSlot);
    switchScenePlaybackOwner(ScenePlaybackOwner::Manual, false);
    setActiveScenePlaybackHandle(activeSceneMainPresetIndex,
                                 activeSceneSlot,
                                 false,
                                 -1,
                                 std::isfinite(hostPpqSnapshot) ? hostPpqSnapshot
                                                                : std::numeric_limits<double>::quiet_NaN(),
                                 getResolvedSceneLengthBeats(activeSceneSlot));
    setSceneChainAttachStartPpq(std::isfinite(hostPpqSnapshot)
                                    ? hostPpqSnapshot
                                    : std::numeric_limits<double>::quiet_NaN());

    const auto synthesizeDefaultSceneOne = [&]()
    {
        constexpr int defaultSceneSlot = 0;
        activeSceneSlot = defaultSceneSlot;
        focusedSceneSlot = defaultSceneSlot;
        focusSceneSlot(defaultSceneSlot);

        for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        {
            clearSceneClipSlotRuntimeState(presetIndex, sceneSlot);
            clearScenePerformanceClip(sceneSlot);
            for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
                clearSceneMotionStripState(sceneSlot, stripIndex);
        }

        if (isSceneModeEnabled() && audioEngine != nullptr)
            syncSceneMotionStateFromEngine(defaultSceneSlot);
        syncScenePerformanceClipLengthToResolvedLength(defaultSceneSlot);

        const bool capturedDefaultScene = captureSceneSlotState(presetIndex,
                                                                defaultSceneSlot,
                                                                true);
        appendSceneDebugLog("perform_preset_load synthesize_default_scene slot=0 preset="
            + juce::String(presetIndex)
            + " captured=" + juce::String(capturedDefaultScene ? 1 : 0)
            + " implicit=1");

        if (capturedDefaultScene)
        {
            clearSceneClipSlotRuntimeState(presetIndex, defaultSceneSlot);
            activeSceneNeedsCaptureBeforeManualRecall = false;
            clearPendingActiveSceneAutosave();
        }
        else
        {
            activeSceneNeedsCaptureBeforeManualRecall = true;
        }

        setActiveScenePlaybackHandle(activeSceneMainPresetIndex,
                                     defaultSceneSlot,
                                     false,
                                     -1,
                                     std::isfinite(hostPpqSnapshot) ? hostPpqSnapshot
                                                                    : std::numeric_limits<double>::quiet_NaN(),
                                     getResolvedSceneLengthBeats(defaultSceneSlot));
    };

    if (!PresetStore::presetExists(presetIndex))
    {
        // Empty slot recall keeps the freshly reset runtime defaults and does
        // not create or mutate preset files, but it should still expose
        // Scene 1 as the active default scene state in-memory.
        synthesizeDefaultSceneOne();
        if (isSceneModeEnabled())
            juce::ignoreUnused(ensureActiveScenePlaybackHandleInitialized());
        resetCurrentBrowserDirectoriesToDefaultPaths(true);
        presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    // Clear stale file references; preset load repopulates file-backed strips.
    for (auto& f : currentStripFiles)
        f = juce::File();

    bool loadSucceeded = false;
    {
        ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
        loadSucceeded = PresetStore::loadPreset(
            presetIndex,
            MaxStrips,
            audioEngine.get(),
            &parameters,
            [this](int stripIndex, const juce::File& sampleFile)
            {
                return loadSampleToStrip(stripIndex, sampleFile);
            },
            {},
            [this](int stripIndex, const juce::File& sampleFile)
            {
                rememberLoadedSamplePathForStrip(stripIndex, sampleFile, false);
            },
            [this](int stripIndex,
                   const juce::File& loopDir,
                   const juce::File& stepDir,
                   const juce::File& flipDir)
            {
                setRecentSampleDirectory(stripIndex, SamplePathMode::Loop, loopDir, false);
                setRecentSampleDirectory(stripIndex, SamplePathMode::Step, stepDir, false);
                setRecentSampleDirectory(stripIndex, SamplePathMode::Flip, flipDir, false);
            },
            [this](int stripIndex, const juce::XmlElement* flipStateXml)
            {
                applyFlipPresetStateXml(stripIndex, flipStateXml);
            },
            [this](int stripIndex, const juce::XmlElement* loopPitchStateXml)
            {
                applyLoopPitchPresetStateXml(stripIndex, loopPitchStateXml);
            },
            [this, presetIndex](const juce::XmlElement& presetXml)
            {
                applySceneChainStateXml(presetXml.getChildByName("SceneChainState"), -1);
                loadStoredSceneSlotStatesForPreset(
                    juce::jlimit(0, MaxPresetSlots - 1, presetIndex), presetXml);
            },
            [this](const juce::MemoryBlock& scenePerformanceData)
            {
                applyScenePerformanceStateData(scenePerformanceData, -1);
            },
            hostPpqSnapshot,
            hostTempoSnapshot,
            true,
            -1,
            PresetStore::StripRecallMode::PresetState,
            nullptr,
            false,
            false);
    }

    GlobalSettingsStore::saveDefaultPaths(*this);

    if (loadSucceeded)
    {
        const bool hasEmbeddedScenes = hasAnyPersistableStoredSceneSlotState(presetIndex);
        bool hasStoredScenes = hasEmbeddedScenes;
        if (!hasStoredScenes)
        {
            clearStoredSceneSlotStates(presetIndex);
            synthesizeDefaultSceneOne();
            hasStoredScenes = hasStoredSceneSlotState(presetIndex, 0);
        }
        normalizeLoopPitchMasterRoles();
        applyLoopPitchSyncToAllStrips();
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        if (isSceneModeEnabled())
        {
            clearAllStripGroupsForSceneMode();
            juce::ignoreUnused(ensureSceneSlotFallbackState(getActiveMainPresetIndexForScenes(), getActiveSceneSlot()));
            juce::ignoreUnused(ensureActiveScenePlaybackHandleInitialized());
        }
        if (!hasStoredScenes)
            activeSceneNeedsCaptureBeforeManualRecall = true;
    }

    if (loadSucceeded && PresetStore::presetExists(presetIndex))
        loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::runPresetSaveRequest(const PresetSaveRequest& request)
{
    if (!audioEngine || request.presetIndex < 0 || request.presetIndex >= MaxPresetSlots)
        return false;

    try
    {
        struct ScopedSuspendProcessing
        {
            explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
            ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
            MlrVSTAudioProcessor& processor;
        } scopedSuspend(*this);

        if (isSceneModeEnabled() && audioEngine != nullptr)
            syncSceneMotionStateFromEngine(activeSceneSlot);
        syncAllScenePerformanceClipLengthsToResolvedLengths();
        const bool includeStoredScenePerformance = hasAnyPersistableStoredSceneSlotState(request.presetIndex);

        return PresetStore::savePreset(request.presetIndex,
                                       MaxStrips,
                                       audioEngine.get(),
                                       parameters,
                                       request.stripFiles.data(),
                                       request.recentLoopDirectories.data(),
                                       request.recentStepDirectories.data(),
                                       request.recentFlipDirectories.data(),
                                       [this](int stripIndex)
                                       {
                                           return createFlipPresetStateXml(stripIndex);
                                       },
                                       [this](int stripIndex)
                                       {
                                           return createLoopPitchPresetStateXml(stripIndex);
                                       },
                                       [this]()
                                       {
                                           return createSceneChainStateXml(-1);
                                       },
                                       includeStoredScenePerformance
                                           ? std::function<juce::MemoryBlock()>(
                                                 [this]()
                                                 {
                                                     return createScenePerformanceStateData(-1);
                                                 })
                                           : std::function<juce::MemoryBlock()>{});
    }
    catch (const std::exception& e)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": unknown");
        return false;
    }
}

void MlrVSTAudioProcessor::pushPresetSaveResult(const PresetSaveResult& result)
{
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        presetSaveResults.push_back(result);
    }
    presetSaveJobsInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::applyCompletedPresetSaves()
{
    std::vector<PresetSaveResult> completed;
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        if (presetSaveResults.empty())
            return;
        completed.swap(presetSaveResults);
    }

    uint32_t successfulSaves = 0;
    for (const auto& result : completed)
    {
        if (!result.success)
        {
            DBG("Preset save failed for slot " << result.presetIndex);
            continue;
        }

        loadedPresetIndex = result.presetIndex;
        ++successfulSaves;
    }

    if (successfulSaves > 0)
        presetRefreshToken.fetch_add(successfulSaves, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::savePreset(int presetIndex)
{
    if (!audioEngine || presetIndex < 0 || presetIndex >= MaxPresetSlots)
        return;

    const int safePresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);
    activeSceneMainPresetIndex = safePresetIndex;

    if (isSceneModeEnabled()
        && hasPersistableStoredSceneSlotState(safePresetIndex, activeSceneSlot))
    {
        juce::ignoreUnused(flushPendingActiveSceneAutosaveIfCurrent());
    }

    if (!isTimerRunning())
        startTimer(kGridRefreshMs);

    PresetSaveRequest request;
    request.presetIndex = safePresetIndex;
    for (int i = 0; i < MaxStrips; ++i)
    {
        request.stripFiles[static_cast<size_t>(i)] = currentStripFiles[static_cast<size_t>(i)];
        request.recentLoopDirectories[static_cast<size_t>(i)] = recentLoopDirectories[static_cast<size_t>(i)];
        request.recentStepDirectories[static_cast<size_t>(i)] = recentStepDirectories[static_cast<size_t>(i)];
        request.recentFlipDirectories[static_cast<size_t>(i)] = recentFlipDirectories[static_cast<size_t>(i)];
    }

    // Keep stored scene ownership aligned with the destination preset before the async
    // save thread snapshots scene state.
    if (storedSceneSlotStateMainPresetIndex >= 0)
    {
        storedSceneSlotStateMainPresetIndex = safePresetIndex;
        for (auto& state : storedSceneSlotStates)
            state.mainPresetIndex = safePresetIndex;
    }
    for (auto& runtimeState : sceneClipSlotRuntimeStates)
    {
        if (runtimeState.hasLiveStripControls)
            runtimeState.mainPresetIndex = safePresetIndex;
    }

    auto* job = new PresetSaveJob(*this, std::move(request));
    presetSaveJobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    presetSaveThreadPool.addJob(job, true);

    // Keep UI/LED state responsive immediately; completion still updates token.
    loadedPresetIndex = safePresetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::loadPreset(int presetIndex)
{
    try
    {
        activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = std::numeric_limits<double>::quiet_NaN();
        const bool hasHostSync = getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot);
        if (!hasHostSync)
        {
            DBG("Preset " << (presetIndex + 1)
                << " loaded without host PPQ/BPM snapshot; recalling audio/parameters only.");
        }

        pendingPresetLoadIndex.store(-1, std::memory_order_release);
        performPresetLoad(presetIndex, hostPpqSnapshot, hostTempoSnapshot);
    }
    catch (const std::exception& e)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": " << e.what());
    }
    catch (...)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": unknown");
    }
}

bool MlrVSTAudioProcessor::deletePreset(int presetIndex)
{
    try
    {
        const bool deleted = PresetStore::deletePreset(presetIndex);
        if (deleted)
        {
            struct ScopedSuspendProcessing
            {
                explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
                ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
                MlrVSTAudioProcessor& processor;
            } scopedSuspend(*this);

            // Deleting any preset slot should leave runtime in a clean state.
            resetRuntimePresetStateToDefaults();
            resetCurrentBrowserDirectoriesToDefaultPaths(true);
            loadedPresetIndex = -1;
            updateMonomeLEDs();
        }
        if (deleted)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return deleted;
    }
    catch (...)
    {
        return false;
    }
}

juce::String MlrVSTAudioProcessor::getPresetName(int presetIndex) const
{
    return PresetStore::getPresetName(presetIndex);
}

bool MlrVSTAudioProcessor::setPresetName(int presetIndex, const juce::String& name)
{
    try
    {
        const bool ok = PresetStore::setPresetName(presetIndex, name);
        if (ok)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }
    catch (...)
    {
        return false;
    }
}

bool MlrVSTAudioProcessor::presetExists(int presetIndex) const
{
    try
    {
        return PresetStore::presetExists(presetIndex);
    }
    catch (...)
    {
        return false;
    }
}
