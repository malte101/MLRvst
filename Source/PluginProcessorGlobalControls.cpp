/*
  ==============================================================================

    PluginProcessorGlobalControls.cpp
    Extracted persistent global control save and UI refresh helpers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "GlobalSettingsStore.h"

void MlrVSTAudioProcessor::flushPersistentGlobalControlsNow()
{
    persistentGlobalUserTouched.store(1, std::memory_order_release);
    persistentGlobalControlsReady.store(1, std::memory_order_release);
    GlobalSettingsStore::saveControlPages(*this);
    persistentGlobalControlsDirty.store(0, std::memory_order_release);
    lastPersistentGlobalControlsSaveMs = juce::Time::currentTimeMillis();
}

void MlrVSTAudioProcessor::requestMonomeLedRefreshAsync()
{
    if (!monomeConnection.supportsGrid())
        return;

    juce::MessageManager::callAsync([this]()
    {
        if (monomeConnection.supportsGrid())
            updateMonomeLEDs();
    });
}

void MlrVSTAudioProcessor::handleUserStripPlayModeChange(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    const bool isFlipMode = strip != nullptr
        && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample;

    ensureSampleModeAudioAvailableForStrip(stripIndex);
    if (isFlipMode)
    {
        if (auto* engine = getSampleModeEngine(stripIndex, true))
            engine->setLegacyLoopEngineEnabled(true);
    }

    reapplyStripStateForCurrentPlayMode(stripIndex);

    if (isSceneModeEnabled())
    {
        syncActiveSceneClipSlotRuntimeStateFromEngine(true);
        queueActiveSceneAutosave();
    }

    requestMonomeLedRefreshAsync();
}

void MlrVSTAudioProcessor::queuePersistentGlobalControlsSave()
{
    if (suppressPersistentGlobalControlsSave.load(std::memory_order_acquire) != 0)
        return;
    if (pendingPersistentGlobalControlsRestore.load(std::memory_order_acquire) != 0)
    {
        if (persistentGlobalUserTouched.load(std::memory_order_acquire) == 0)
            return;
        pendingPersistentGlobalControlsRestore.store(0, std::memory_order_release);
        pendingPersistentGlobalControlsRestoreRemaining = 0;
    }
    if (persistentGlobalControlsReady.load(std::memory_order_acquire) == 0)
        return;

    persistentGlobalControlsDirty.store(1, std::memory_order_release);
}
