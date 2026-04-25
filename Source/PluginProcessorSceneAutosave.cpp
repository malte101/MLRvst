/*
  ==============================================================================

    PluginProcessorSceneAutosave.cpp
    Extracted active scene autosave helpers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SceneScheduler.h"

namespace
{
bool isValidSceneStoredSamplePath(const juce::File& file)
{
    const auto path = file.getFullPathName().trim();
    if (path.isEmpty() || path.length() > 4096)
        return false;
    if (!juce::File::isAbsolutePath(path))
        return false;
    if (path.contains("\n") || path.contains("\r") || path.contains("://"))
        return false;
    if (path.startsWith("//") || path.startsWith("\\\\"))
        return false;
    return file.hasFileExtension("wav;aif;aiff;mp3;ogg;flac");
}

bool canEmbedSceneAudioBuffer(const juce::AudioBuffer<float>& buffer)
{
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return false;

    constexpr uint64_t kMaxEmbeddedWavBytes = 48ULL * 1024ULL * 1024ULL;
    const uint64_t estimatedWavBytes = static_cast<uint64_t>(channels)
        * static_cast<uint64_t>(samples)
        * 3ULL
        + 4096ULL;
    return estimatedWavBytes <= kMaxEmbeddedWavBytes;
}
} // namespace

void MlrVSTAudioProcessor::clearPendingActiveSceneAutosave()
{
    pendingActiveSceneAutosaveDirty.store(0, std::memory_order_release);
    pendingActiveSceneAutosaveMainPreset.store(-1, std::memory_order_release);
    pendingActiveSceneAutosaveSlot.store(-1, std::memory_order_release);
    pendingActiveSceneAutosaveQueuedMs.store(0, std::memory_order_release);
}

bool MlrVSTAudioProcessor::canPersistActiveSceneSnapshotSafely() const
{
    if (audioEngine == nullptr)
        return false;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto playMode = strip->getPlayMode();
        if (!(strip->hasAudio() || playMode == EnhancedAudioStrip::PlayMode::Sample))
            continue;

        const auto& sourceFile = currentStripFiles[static_cast<size_t>(stripIndex)];
        if (isValidSceneStoredSamplePath(sourceFile))
            continue;

        const auto* audioBuffer = strip->getAudioBuffer();
        if (audioBuffer != nullptr && canEmbedSceneAudioBuffer(*audioBuffer))
            continue;

        return false;
    }

    return true;
}

bool MlrVSTAudioProcessor::refreshStoredSceneSlotSnapshot(int mainPresetIndex, int sceneSlot)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const auto* storedSceneState = getStoredSceneSlotState(safeMainPresetIndex, safeSceneSlot);
    if (storedSceneState == nullptr)
        return false;

    if (!storedSceneState->implicitMainPresetFallback)
        return SceneScheduler::saveSceneForMainPreset(*this, safeMainPresetIndex, safeSceneSlot);

    if (!audioEngine)
        return false;

    if (isSceneModeEnabled() && safeSceneSlot == getActiveSceneSlot())
        syncSceneMotionStateFromEngine(safeSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);

    if (!captureSceneSlotState(safeMainPresetIndex, safeSceneSlot, true))
        return false;

    clearSceneClipSlotRuntimeState(safeMainPresetIndex, safeSceneSlot);
    if (safeSceneSlot == activeSceneSlot
        && safeMainPresetIndex
            == juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex))
    {
        activeSceneNeedsCaptureBeforeManualRecall = false;
        clearPendingActiveSceneAutosave();
    }

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool MlrVSTAudioProcessor::flushPendingActiveSceneAutosaveIfCurrent()
{
    if (pendingActiveSceneAutosaveDirty.load(std::memory_order_acquire) == 0)
        return true;
    if (isSceneAutosaveSuppressed())
        return false;

    const int queuedMainPreset = pendingActiveSceneAutosaveMainPreset.load(std::memory_order_acquire);
    const int queuedSceneSlot = pendingActiveSceneAutosaveSlot.load(std::memory_order_acquire);
    const int currentMainPreset = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
    const int currentSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (queuedMainPreset != currentMainPreset || queuedSceneSlot != currentSceneSlot)
        return true;
    if (!canPersistActiveSceneSnapshotSafely())
        return false;

    return refreshStoredSceneSlotSnapshot(currentMainPreset, currentSceneSlot);
}

void MlrVSTAudioProcessor::processPendingSceneAutosave()
{
    if (pendingActiveSceneAutosaveDirty.load(std::memory_order_acquire) == 0)
        return;
    SceneSwitchEvent pendingSwitchEvent;
    if (isSceneAutosaveSuppressed()
        || peekPendingSceneApplyState(pendingSwitchEvent)
        || scenePerformanceRecorder.isRecording())
    {
        return;
    }

    const int queuedMainPreset = pendingActiveSceneAutosaveMainPreset.load(std::memory_order_acquire);
    const int queuedSceneSlot = pendingActiveSceneAutosaveSlot.load(std::memory_order_acquire);
    const int currentMainPreset = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
    const int currentSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (queuedMainPreset != currentMainPreset || queuedSceneSlot != currentSceneSlot)
        return;

    const uint32_t queuedMs = pendingActiveSceneAutosaveQueuedMs.load(std::memory_order_acquire);
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    if (queuedMs != 0 && static_cast<int32_t>(nowMs - queuedMs) < 0)
        return;

    flushPendingActiveSceneAutosaveIfCurrent();
}

void MlrVSTAudioProcessor::beginSceneAutosaveSuppression()
{
    sceneAutosaveSuppressionDepth.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::endSceneAutosaveSuppression()
{
    sceneAutosaveSuppressionDepth.fetch_sub(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::isSceneAutosaveSuppressed() const
{
    return sceneAutosaveSuppressionDepth.load(std::memory_order_acquire) > 0;
}
