#include "PluginProcessor.h"
#include "PresetStore.h"

int MlrVSTAudioProcessor::getActiveMainPresetIndexForScenes() const
{
    if (loadedPresetIndex >= 0 && loadedPresetIndex < MaxPresetSlots)
        return loadedPresetIndex;
    return juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
}

int MlrVSTAudioProcessor::getSceneStoragePresetIndex(int mainPresetIndex, int sceneSlot) const
{
    const int clampedMain = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    return MaxPresetSlots + (clampedMain * SceneSlots) + clampedSlot;
}

bool MlrVSTAudioProcessor::hasStoredSceneSlotState(int mainPresetIndex, int sceneSlot) const
{
    return getStoredSceneSlotState(mainPresetIndex, sceneSlot) != nullptr;
}

const MlrVSTAudioProcessor::SceneSlotState* MlrVSTAudioProcessor::getStoredSceneSlotState(int mainPresetIndex,
                                                                                          int sceneSlot) const
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (storedSceneSlotStateMainPresetIndex != safeMainPresetIndex)
        return nullptr;

    const auto& state = storedSceneSlotStates[static_cast<size_t>(safeSceneSlot)];
    if (!state.hasStoredContent
        || state.mainPresetIndex != safeMainPresetIndex
        || state.preparedSwitchPayloadTemplate == nullptr)
    {
        return nullptr;
    }

    return &state;
}

void MlrVSTAudioProcessor::clearStoredSceneSlotStates(int mainPresetIndex)
{
    storedSceneSlotStateMainPresetIndex = mainPresetIndex >= 0
        ? juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        : -1;

    for (int slot = 0; slot < SceneSlots; ++slot)
    {
        auto& state = storedSceneSlotStates[static_cast<size_t>(slot)];
        state = {};
        state.mainPresetIndex = storedSceneSlotStateMainPresetIndex >= 0
            ? storedSceneSlotStateMainPresetIndex
            : 0;
        state.sceneSlot = slot;
    }
}

bool MlrVSTAudioProcessor::loadStoredSceneSlotStatesForPreset(int mainPresetIndex,
                                                              const juce::XmlElement& presetXml)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    if (restoreStoredSceneSlotStatesFromPresetXml(safeMainPresetIndex, presetXml))
        return true;

    const bool migrated = migrateLegacyStoredSceneSlotStates(safeMainPresetIndex);
    if (migrated)
        juce::ignoreUnused(persistStoredSceneSlotStatesToMainPreset(safeMainPresetIndex));
    return migrated;
}

bool MlrVSTAudioProcessor::restoreStoredSceneSlotStatesFromPresetXml(int mainPresetIndex,
                                                                     const juce::XmlElement& presetXml)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    clearStoredSceneSlotStates(safeMainPresetIndex);

    const auto* sceneTimingXml = presetXml.getChildByName("SceneChainState");
    if (sceneTimingXml == nullptr)
        return false;

    const auto* storedScenesXml = sceneTimingXml->getChildByName("StoredScenes");
    if (storedScenesXml == nullptr)
        return false;

    bool restoredAny = false;
    for (auto* sceneSlotXml : storedScenesXml->getChildIterator())
    {
        if (sceneSlotXml == nullptr || sceneSlotXml->getTagName() != "SceneSlot")
            continue;

        const int safeSceneSlot = juce::jlimit(0,
                                               SceneSlots - 1,
                                               sceneSlotXml->getIntAttribute("slot", -1));
        auto* snapshotXml = sceneSlotXml->getChildByName("mlrVSTPreset");
        if (snapshotXml == nullptr)
            continue;

        auto& state = storedSceneSlotStates[static_cast<size_t>(safeSceneSlot)];
        state.mainPresetIndex = safeMainPresetIndex;
        state.sceneSlot = safeSceneSlot;
        state.hasStoredContent = true;
        state.name = sceneSlotXml->getStringAttribute("name",
                                                      snapshotXml->getStringAttribute("name").trim()).trim();
        auto templatePayload = std::make_unique<PreparedSceneSwitchPayload>();
        if (!parsePreparedSceneSwitchPayloadTemplate(*templatePayload,
                                                     *snapshotXml,
                                                     safeMainPresetIndex,
                                                     safeSceneSlot))
        {
            state = {};
            state.mainPresetIndex = safeMainPresetIndex;
            state.sceneSlot = safeSceneSlot;
            continue;
        }
        state.preparedSwitchPayloadTemplate = std::move(templatePayload);
        restoredAny = true;
    }

    return restoredAny;
}

bool MlrVSTAudioProcessor::migrateLegacyStoredSceneSlotStates(int mainPresetIndex)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    clearStoredSceneSlotStates(safeMainPresetIndex);

    bool migratedAny = false;
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
    {
        const int storageIndex = getSceneStoragePresetIndex(safeMainPresetIndex, sceneSlot);
        auto legacySceneXml = PresetStore::loadPresetXml(storageIndex);
        if (legacySceneXml == nullptr)
            continue;

        juce::MemoryBlock legacyScenePerformanceData;
        if (PresetStore::loadScenePerformanceData(storageIndex, legacyScenePerformanceData)
            && legacyScenePerformanceData.getSize() > 0
            && legacySceneXml->getChildByName("ScenePerformanceData") == nullptr)
        {
            auto* perfXml = legacySceneXml->createNewChildElement("ScenePerformanceData");
            perfXml->addTextElement(legacyScenePerformanceData.toBase64Encoding());
        }

        auto& state = storedSceneSlotStates[static_cast<size_t>(sceneSlot)];
        state.mainPresetIndex = safeMainPresetIndex;
        state.sceneSlot = sceneSlot;
        state.hasStoredContent = true;
        state.name = legacySceneXml->getStringAttribute("name").trim();
        auto templatePayload = std::make_unique<PreparedSceneSwitchPayload>();
        if (!parsePreparedSceneSwitchPayloadTemplate(*templatePayload,
                                                     *legacySceneXml,
                                                     safeMainPresetIndex,
                                                     sceneSlot))
        {
            state = {};
            state.mainPresetIndex = safeMainPresetIndex;
            state.sceneSlot = sceneSlot;
            continue;
        }
        state.preparedSwitchPayloadTemplate = std::move(templatePayload);
        migratedAny = true;
    }

    return migratedAny;
}

bool MlrVSTAudioProcessor::captureSceneSlotState(int mainPresetIndex, int sceneSlot)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);

    juce::String sceneName;
    if (const auto* existingState = getStoredSceneSlotState(safeMainPresetIndex, safeSceneSlot))
        sceneName = existingState->name.trim();
    if (sceneName.isEmpty())
        sceneName = getSceneInfo(safeSceneSlot, safeMainPresetIndex).name.trim();

    if (storedSceneSlotStateMainPresetIndex != safeMainPresetIndex)
        clearStoredSceneSlotStates(safeMainPresetIndex);

    auto& state = storedSceneSlotStates[static_cast<size_t>(safeSceneSlot)];
    state = {};
    state.mainPresetIndex = safeMainPresetIndex;
    state.sceneSlot = safeSceneSlot;
    state.hasStoredContent = true;
    state.name = sceneName;
    auto templatePayload = std::make_unique<PreparedSceneSwitchPayload>();
    if (!capturePreparedSceneSwitchPayloadTemplate(*templatePayload,
                                                   safeMainPresetIndex,
                                                   safeSceneSlot))
    {
        state = {};
        state.mainPresetIndex = safeMainPresetIndex;
        state.sceneSlot = safeSceneSlot;
        return false;
    }
    state.preparedSwitchPayloadTemplate = std::move(templatePayload);
    return true;
}

bool MlrVSTAudioProcessor::copyStoredSceneSlotState(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSourceSlot = juce::jlimit(0, SceneSlots - 1, sourceSceneSlot);
    const int safeDestSlot = juce::jlimit(0, SceneSlots - 1, destSceneSlot);

    if (storedSceneSlotStateMainPresetIndex != safeMainPresetIndex)
        clearStoredSceneSlotStates(safeMainPresetIndex);

    auto& destState = storedSceneSlotStates[static_cast<size_t>(safeDestSlot)];
    destState = {};
    destState.mainPresetIndex = safeMainPresetIndex;
    destState.sceneSlot = safeDestSlot;

    const auto* sourceState = getStoredSceneSlotState(safeMainPresetIndex, safeSourceSlot);
    if (sourceState == nullptr)
        return true;

    destState.hasStoredContent = true;
    destState.name = sourceState->name;
    destState.preparedSwitchPayloadTemplate =
        clonePreparedSceneSwitchPayloadTemplate(*sourceState->preparedSwitchPayloadTemplate);
    if (destState.preparedSwitchPayloadTemplate != nullptr)
    {
        destState.preparedSwitchPayloadTemplate->mainPresetIndex = safeMainPresetIndex;
        destState.preparedSwitchPayloadTemplate->sceneSlot = safeDestSlot;
        destState.preparedSwitchPayloadTemplate->sequenceDriven = false;
        destState.preparedSwitchPayloadTemplate->sequenceStepIndex = -1;
        destState.preparedSwitchPayloadTemplate->switchSerial = 0;
        destState.preparedSwitchPayloadTemplate->snapshotPresetXml.reset();
    }
    return true;
}

bool MlrVSTAudioProcessor::deleteStoredSceneSlotState(int mainPresetIndex, int sceneSlot)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);

    if (storedSceneSlotStateMainPresetIndex != safeMainPresetIndex)
        clearStoredSceneSlotStates(safeMainPresetIndex);

    auto& state = storedSceneSlotStates[static_cast<size_t>(safeSceneSlot)];
    state = {};
    state.mainPresetIndex = safeMainPresetIndex;
    state.sceneSlot = safeSceneSlot;
    return true;
}

bool MlrVSTAudioProcessor::persistStoredSceneSlotStatesToMainPreset(int mainPresetIndex)
{
    return PresetStore::updatePresetAuxState(juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex),
                                             [this]()
                                             {
                                                 return createSceneChainStateXml(-1);
                                             });
}
