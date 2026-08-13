/*
  ==============================================================================

    GlobalSettingsStore.cpp
    Persistent default-path and global-settings serialization helpers

  ==============================================================================
*/

#include "GlobalSettingsStore.h"
#include "PluginProcessor.h"
#include <cmath>

namespace
{
constexpr int kPitchControlModeSchemaVersion = 3;
constexpr const char* kGlobalSettingsKey = "GlobalSettingsXml";

juce::File restoreStoredAbsolutePath(const juce::String& rawPath)
{
    const auto path = rawPath.trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return {};
    return juce::File(path);
}

juce::File getGlobalSettingsFile()
{
    auto presetsRoot = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library")
        .getChildFile("Audio")
        .getChildFile("Presets")
        .getChildFile("mlrVST")
        .getChildFile("mlrVST");
    return presetsRoot.getChildFile("GlobalSettings.xml");
}

juce::File getLegacyGlobalSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("GlobalSettings.xml");
}

juce::PropertiesFile::Options getLegacySettingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "mlrVST";
    options.filenameSuffix = "settings";
    options.folderName = "";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
}

int migrateLegacyPitchControlModeIndex(int storedIndex) noexcept
{
    switch (storedIndex)
    {
        case 1: return 3;
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        case 0:
        default:
            return 0;
    }
}

int sanitizePitchControlModeIndex(int storedIndex) noexcept
{
    return juce::jlimit(0, 4, storedIndex);
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

std::unique_ptr<juce::XmlElement> loadGlobalSettingsXml()
{
    auto settingsFile = getGlobalSettingsFile();
    if (settingsFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(settingsFile))
            return xml;
    }

    auto legacySettingsFile = getLegacyGlobalSettingsFile();
    if (legacySettingsFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(legacySettingsFile))
            return xml;
    }

    juce::PropertiesFile legacyProps(getLegacySettingsOptions());
    if (legacyProps.isValidFile())
        return legacyProps.getXmlValue(kGlobalSettingsKey);

    return nullptr;
}

void saveGlobalSettingsXml(const juce::XmlElement& xml)
{
    auto settingsFile = getGlobalSettingsFile();
    auto settingsDir = settingsFile.getParentDirectory();
    if (!settingsDir.exists())
        settingsDir.createDirectory();
    xml.writeTo(settingsFile);

    auto legacyFile = getLegacyGlobalSettingsFile();
    auto legacyDir = legacyFile.getParentDirectory();
    if (!legacyDir.exists())
        legacyDir.createDirectory();
    if (legacyFile != settingsFile)
        xml.writeTo(legacyFile);

    juce::PropertiesFile legacyProps(getLegacySettingsOptions());
    if (legacyProps.isValidFile())
    {
        legacyProps.setValue(kGlobalSettingsKey, &xml);
        legacyProps.saveIfNeeded();
    }
}
} // namespace

void GlobalSettingsStore::loadDefaultPaths(MlrVSTAudioProcessor& owner)
{
    auto settingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("DefaultPaths.xml");

    if (!settingsFile.existsAsFile())
    {
        owner.resetCurrentBrowserDirectoriesToDefaultPaths(false);
        saveDefaultPaths(owner);
        return;
    }

    auto xml = juce::XmlDocument::parse(settingsFile);
    if (xml == nullptr || xml->getTagName() != "DefaultPaths")
    {
        owner.resetCurrentBrowserDirectoriesToDefaultPaths(false);
        saveDefaultPaths(owner);
        return;
    }

    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        owner.defaultLoopDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("loopDir" + juce::String(i)));
        owner.defaultStepDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("stepDir" + juce::String(i)));
        owner.defaultFlipDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("flipDir" + juce::String(i)));
        owner.recentLoopDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentLoopDir" + juce::String(i)));
        owner.recentStepDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentStepDir" + juce::String(i)));
        owner.recentFlipDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentFlipDir" + juce::String(i)));
    }

    owner.lastSampleFolder = restoreStoredAbsolutePath(xml->getStringAttribute("lastSampleFolder"));

    for (int slot = 0; slot < MlrVSTAudioProcessor::BrowserFavoriteSlots; ++slot)
    {
        owner.browserFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(xml->getStringAttribute("favoriteDir" + juce::String(slot)));
        owner.browserFlipFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(xml->getStringAttribute("favoriteFlipDir" + juce::String(slot)));
    }

    owner.resetCurrentBrowserDirectoriesToDefaultPaths(false);
}

void GlobalSettingsStore::saveDefaultPaths(const MlrVSTAudioProcessor& owner)
{
    auto settingsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST");
    if (!settingsDir.exists())
        settingsDir.createDirectory();

    auto settingsFile = settingsDir.getChildFile("DefaultPaths.xml");
    juce::XmlElement xml("DefaultPaths");

    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        xml.setAttribute("loopDir" + juce::String(i), owner.defaultLoopDirectories[idx].getFullPathName());
        xml.setAttribute("stepDir" + juce::String(i), owner.defaultStepDirectories[idx].getFullPathName());
        xml.setAttribute("flipDir" + juce::String(i), owner.defaultFlipDirectories[idx].getFullPathName());
        xml.setAttribute("recentLoopDir" + juce::String(i), owner.recentLoopDirectories[idx].getFullPathName());
        xml.setAttribute("recentStepDir" + juce::String(i), owner.recentStepDirectories[idx].getFullPathName());
        xml.setAttribute("recentFlipDir" + juce::String(i), owner.recentFlipDirectories[idx].getFullPathName());
    }

    xml.setAttribute("lastSampleFolder", owner.lastSampleFolder.getFullPathName());

    for (int slot = 0; slot < MlrVSTAudioProcessor::BrowserFavoriteSlots; ++slot)
    {
        xml.setAttribute("favoriteDir" + juce::String(slot),
                         owner.browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName());
        xml.setAttribute("favoriteFlipDir" + juce::String(slot),
                         owner.browserFlipFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName());
    }

    xml.writeTo(settingsFile);
}

void GlobalSettingsStore::loadControlPages(MlrVSTAudioProcessor& owner)
{
    const auto previousSuppress = owner.suppressPersistentGlobalControlsSave.load(std::memory_order_acquire);
    owner.suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
    auto xml = loadGlobalSettingsXml();
    if (xml == nullptr || xml->getTagName() != "GlobalSettings")
    {
        owner.suppressPersistentGlobalControlsSave.store(previousSuppress, std::memory_order_release);
        saveControlPages(owner);
        return;
    }

    juce::ValueTree state("MlrVST");
    auto controlPages = juce::ValueTree("ControlPages");
    for (int i = 0; i < MlrVSTAudioProcessor::NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, xml->getStringAttribute(key), nullptr);
    }
    controlPages.setProperty("momentary", xml->getBoolAttribute("momentary", true), nullptr);
    controlPages.setProperty("swingDivision", xml->getIntAttribute("swingDivision", 1), nullptr);
    state.addChild(controlPages, -1, nullptr);

    owner.loadControlPagesFromState(state);
    owner.suppressPersistentGlobalControlsSave.store(previousSuppress, std::memory_order_release);
}

void GlobalSettingsStore::loadGlobalControls(MlrVSTAudioProcessor& owner)
{
    auto xml = loadGlobalSettingsXml();
    if (xml == nullptr || xml->getTagName() != "GlobalSettings")
    {
        owner.persistentGlobalControlsReady.store(1, std::memory_order_release);
        return;
    }

    auto restoreFloatParam = [&owner, &xml](const char* attrName, const char* paramId, double minValue, double maxValue) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = owner.parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        const auto restored = static_cast<float>(
            juce::jlimit(minValue, maxValue, xml->getDoubleAttribute(attrName)));
        param->setValueNotifyingHost(param->convertTo0to1(restored));
        return true;
    };

    auto restoreChoiceParam = [&owner, &xml](const char* attrName, const char* paramId, int minValue, int maxValue) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = owner.parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        const auto restored = static_cast<float>(
            juce::jlimit(minValue, maxValue, xml->getIntAttribute(attrName)));
        param->setValueNotifyingHost(param->convertTo0to1(restored));
        return true;
    };

    auto restorePitchControlModeParam = [&owner, &xml]() -> bool
    {
        if (!xml->hasAttribute("pitchControlMode"))
            return false;

        auto* param = owner.parameters.getParameter("pitchControlMode");
        if (param == nullptr)
            return false;

        int restored = xml->getIntAttribute("pitchControlMode");
        if (xml->getIntAttribute("pitchControlModeSchemaVersion", 0) < kPitchControlModeSchemaVersion)
            restored = migrateLegacyPitchControlModeIndex(restored);

        restored = sanitizePitchControlModeIndex(restored);
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(restored)));
        return true;
    };

    auto restoreBoolParam = [&owner, &xml](const char* attrName, const char* paramId) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = owner.parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        param->setValueNotifyingHost(param->convertTo0to1(xml->getBoolAttribute(attrName) ? 1.0f : 0.0f));
        return true;
    };

    owner.suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
    bool anyRestored = false;

    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
    {
        const auto ccAttrName = "macroCc" + juce::String(i);
        if (xml->hasAttribute(ccAttrName))
        {
            const int restoredCc = juce::jlimit(-1, 127, xml->getIntAttribute(ccAttrName, owner.getDefaultMacroMidiCc(i)));
            owner.macroMidiCcAssignments[static_cast<size_t>(i)].store(restoredCc, std::memory_order_release);
            anyRestored = true;
        }

        const auto targetAttrName = "macroTarget" + juce::String(i);
        const auto targetKeyAttrName = "macroTargetKey" + juce::String(i);
        bool restoredMacroTarget = false;
        if (xml->hasAttribute(targetKeyAttrName))
        {
            PerformanceTarget restoredTarget = PerformanceTarget::None;
            if (tryParsePerformanceTargetKey(xml->getStringAttribute(targetKeyAttrName), restoredTarget))
            {
                owner.macroTargetAssignments[static_cast<size_t>(i)].store(
                    static_cast<int>(sanitizeMacroPerformanceTarget(restoredTarget)),
                    std::memory_order_release);
                anyRestored = true;
                restoredMacroTarget = true;
            }
        }
        if (!restoredMacroTarget && xml->hasAttribute(targetAttrName))
        {
            const auto restoredTarget = performanceTargetFromLegacyMacroRaw(
                xml->getIntAttribute(targetAttrName,
                                     legacyMacroRawFromPerformanceTarget(owner.getDefaultMacroTarget(i))));
            owner.macroTargetAssignments[static_cast<size_t>(i)].store(
                static_cast<int>(sanitizeMacroPerformanceTarget(restoredTarget)),
                std::memory_order_release);
            anyRestored = true;
        }
    }

    owner.macroTargetAssignments[5].store(static_cast<int>(owner.getDefaultMacroTarget(5)), std::memory_order_release);
    owner.macroTargetAssignments[6].store(static_cast<int>(owner.getDefaultMacroTarget(6)), std::memory_order_release);
    owner.macroTargetAssignments[7].store(static_cast<int>(owner.getDefaultMacroTarget(7)), std::memory_order_release);

    if (xml->hasAttribute("rootNoteMidi"))
    {
        owner.globalRootNoteMidi.store(juce::jlimit(0, 127, xml->getIntAttribute("rootNoteMidi", 60)),
                                       std::memory_order_release);
        anyRestored = true;
    }
    if (xml->hasAttribute("pitchScale"))
    {
        owner.globalPitchScale.store(
            juce::jlimit(0,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         xml->getIntAttribute("pitchScale", static_cast<int>(ModernAudioEngine::PitchScale::Chromatic))),
            std::memory_order_release);
        anyRestored = true;
    }

    owner.cancelMacroMidiLearn();
    // masterVolume, quantize, and inputMonitor are session state, not preferences:
    // fresh instances always start at their factory defaults (1.0 / 1-16 / 1.0).
    anyRestored = restoreFloatParam("limiterThreshold", "limiterThreshold", 0.0, 0.0) || anyRestored;
    anyRestored = restoreBoolParam("limiterEnabled", "limiterEnabled") || anyRestored;
    anyRestored = restoreChoiceParam("innerLoopLength", "innerLoopLength", 0, 4) || anyRestored;
    owner.innerLoopLengthSelection.store(owner.innerLoopLengthParam != nullptr
                                             ? juce::jlimit(0, 4, static_cast<int>(owner.innerLoopLengthParam->load(std::memory_order_acquire)))
                                             : 0,
                                         std::memory_order_release);
    owner.lastAppliedInnerLoopLengthSelection.store(owner.innerLoopLengthSelection.load(std::memory_order_acquire),
                                                    std::memory_order_release);
    anyRestored = restoreChoiceParam("quality", "quality", 0, 3) || anyRestored;
    anyRestored = restoreFloatParam("pitchSmoothing", "pitchSmoothing", 0.0, 1.0) || anyRestored;
    anyRestored = restoreFloatParam("crossfadeLength", "crossfadeLength", 1.0, 50.0) || anyRestored;
    anyRestored = restoreFloatParam("triggerFadeIn", "triggerFadeIn", 0.01, 120.0) || anyRestored;
    anyRestored = restoreChoiceParam("outputRouting", "outputRouting", 0, 1) || anyRestored;
    anyRestored = restorePitchControlModeParam() || anyRestored;
    anyRestored = restoreChoiceParam("flipTempoMatchMode", "flipTempoMatchMode", 0, 1) || anyRestored;
    bool stretchBackendRestored = restoreChoiceParam("stretchBackend", "stretchBackend", 0, 2);
    anyRestored = restoreBoolParam("sceneMode", "sceneMode") || anyRestored;
    anyRestored = restoreChoiceParam("transientOnsetMethod", "transientOnsetMethod", 0, 2) || anyRestored;
    anyRestored = restoreChoiceParam("transientSensitivity", "transientSensitivity", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("transientSnap", "transientSnap", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("transientSpacing", "transientSpacing", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("sceneRecallMode", "sceneRecallMode", 0, 3) || anyRestored;
    owner.setSceneRecallMode(MlrVSTAudioProcessor::SceneRecallMode::Manual);
    xml->setAttribute("sceneRecallMode",
                      static_cast<int>(MlrVSTAudioProcessor::SceneRecallMode::Manual));
    if (xml->hasAttribute("sceneEditorGridEnabled"))
    {
        owner.sceneEditorGridEnabledState = xml->getBoolAttribute("sceneEditorGridEnabled", true);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorGridDivision"))
    {
        owner.sceneEditorGridDivisionState = juce::jlimit(1, 64, xml->getIntAttribute("sceneEditorGridDivision", 16));
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorDrawMode"))
    {
        owner.sceneEditorDrawModeEnabledState = xml->getBoolAttribute("sceneEditorDrawMode", false);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorLaneOverlays"))
    {
        owner.sceneEditorLaneOverlaysEnabledState = xml->getBoolAttribute("sceneEditorLaneOverlays", true);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorZoomFactor"))
    {
        const int restoredZoom = xml->getIntAttribute("sceneEditorZoomFactor", 1);
        static constexpr std::array<int, 7> kZoomChoices{ 1, 2, 3, 4, 5, 6, 8 };
        owner.sceneEditorZoomFactorState = kZoomChoices.front();
        int bestDelta = std::abs(restoredZoom - owner.sceneEditorZoomFactorState);
        for (const int candidate : kZoomChoices)
        {
            const int delta = std::abs(restoredZoom - candidate);
            if (delta < bestDelta)
            {
                bestDelta = delta;
                owner.sceneEditorZoomFactorState = candidate;
            }
        }
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorFollowPlayhead"))
    {
        owner.sceneEditorFollowPlayheadState = xml->getBoolAttribute("sceneEditorFollowPlayhead", false);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneModPageMode"))
    {
        owner.sceneModPageModeState = static_cast<MlrVSTAudioProcessor::SceneModPageMode>(juce::jlimit(
            0,
            static_cast<int>(MlrVSTAudioProcessor::SceneModPageMode::MainModulation),
            xml->getIntAttribute("sceneModPageMode",
                                 static_cast<int>(MlrVSTAudioProcessor::SceneModPageMode::StepMotion))));
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneTransitionEndSampleDir")
        && (owner.sceneChainState.transitionEndSampleDirectory == juce::File()
            || owner.sceneChainState.transitionEndSampleDirectory.getFullPathName().trim().isEmpty()))
    {
        const auto endSampleDirectoryPath = xml->getStringAttribute("sceneTransitionEndSampleDir").trim();
        owner.sceneChainState.transitionEndSampleDirectory =
            endSampleDirectoryPath.isEmpty() ? juce::File() : juce::File(endSampleDirectoryPath);
        anyRestored = true;
    }
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        const auto automationKey = "sceneStripAutomationExpanded" + juce::String(stripIndex);
        if (xml->hasAttribute(automationKey))
        {
            owner.sceneEditorStripAutomationExpanded[static_cast<size_t>(stripIndex)] =
                xml->getBoolAttribute(automationKey, true);
            anyRestored = true;
        }

        const auto heightKey = "sceneStripHeightExpanded" + juce::String(stripIndex);
        if (xml->hasAttribute(heightKey))
        {
            owner.sceneEditorStripHeightExpanded[static_cast<size_t>(stripIndex)] =
                xml->getBoolAttribute(heightKey, false);
            anyRestored = true;
        }
    }
    if (!stretchBackendRestored && xml->hasAttribute("soundTouchEnabled"))
    {
        auto* param = owner.parameters.getParameter("stretchBackend");
        if (param != nullptr)
        {
            const int restoredBackend = xml->getBoolAttribute("soundTouchEnabled")
                ? static_cast<int>(TimeStretchBackend::SoundTouch)
                : static_cast<int>(TimeStretchBackend::Resample);
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(restoredBackend)));
            stretchBackendRestored = true;
        }
    }
    anyRestored = stretchBackendRestored || anyRestored;
    const bool restoredLegacyGestures =
        owner.gestureCoordinator != nullptr ? owner.gestureCoordinator->restoreProfilesFromXml(*xml) : false;
    if (owner.gestureCoordinator != nullptr)
        owner.gestureCoordinator->expandLegacyProfilesToCombos();
    const bool restoredExactGestureCombos =
        owner.gestureCoordinator != nullptr ? owner.gestureCoordinator->restoreComboProfilesFromXml(*xml) : false;
    anyRestored = restoredExactGestureCombos || restoredLegacyGestures || anyRestored;
    if (owner.soundTouchEnabledParam != nullptr)
    {
        const bool legacySoundTouchEnabled = owner.getStretchBackend() != TimeStretchBackend::Resample;
        auto* legacyParam = owner.parameters.getParameter("soundTouchEnabled");
        if (legacyParam != nullptr)
            legacyParam->setValueNotifyingHost(legacyParam->convertTo0to1(legacySoundTouchEnabled ? 1.0f : 0.0f));
    }
    owner.suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
    owner.persistentGlobalControlsDirty.store(0, std::memory_order_release);

    if (!anyRestored)
        saveControlPages(owner);
    else
        saveGlobalSettingsXml(*xml);

    owner.syncSceneModeFromParameters();
    owner.applyLoopPitchSyncToAllStrips();
    owner.syncTransientDetectionSettingsFromParameters(true);
    if (owner.gestureCoordinator)
        owner.gestureCoordinator->syncToAudioEngine();
    owner.persistentGlobalControlsReady.store(1, std::memory_order_release);
}

void GlobalSettingsStore::saveControlPages(const MlrVSTAudioProcessor& owner)
{
    if (owner.suppressPersistentGlobalControlsSave.load(std::memory_order_acquire) != 0)
        return;

    juce::XmlElement xml("GlobalSettings");
    const auto orderSnapshot = owner.getControlPageOrder();
    for (int i = 0; i < MlrVSTAudioProcessor::NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        xml.setAttribute(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]));
    }
    xml.setAttribute("momentary", owner.isControlPageMomentary());
    xml.setAttribute("swingDivision", owner.swingDivisionSelection.load(std::memory_order_acquire));
    if (owner.limiterThresholdParam)
        xml.setAttribute("limiterThreshold", 0.0);
    if (owner.limiterEnabledParam)
        xml.setAttribute("limiterEnabled", owner.limiterEnabledParam->load(std::memory_order_acquire) >= 0.5f);
    if (owner.innerLoopLengthParam)
        xml.setAttribute("innerLoopLength", static_cast<int>(owner.innerLoopLengthParam->load(std::memory_order_acquire)));
    if (owner.grainQualityParam)
        xml.setAttribute("quality", static_cast<int>(owner.grainQualityParam->load(std::memory_order_acquire)));
    if (owner.pitchSmoothingParam)
        xml.setAttribute("pitchSmoothing", static_cast<double>(owner.pitchSmoothingParam->load(std::memory_order_acquire)));
    if (owner.crossfadeLengthParam)
        xml.setAttribute("crossfadeLength", static_cast<double>(owner.crossfadeLengthParam->load(std::memory_order_acquire)));
    if (owner.triggerFadeInParam)
        xml.setAttribute("triggerFadeIn", static_cast<double>(owner.triggerFadeInParam->load(std::memory_order_acquire)));
    if (owner.outputRoutingParam)
        xml.setAttribute("outputRouting", static_cast<int>(owner.outputRoutingParam->load(std::memory_order_acquire)));
    if (owner.pitchControlModeParam)
    {
        const int storedMode = sanitizePitchControlModeIndex(
            static_cast<int>(std::round(owner.pitchControlModeParam->load(std::memory_order_acquire))));
        xml.setAttribute("pitchControlMode", storedMode);
        xml.setAttribute("pitchControlModeSchemaVersion", kPitchControlModeSchemaVersion);
    }
    if (owner.flipTempoMatchModeParam)
        xml.setAttribute("flipTempoMatchMode", static_cast<int>(owner.flipTempoMatchModeParam->load(std::memory_order_acquire)));
    if (owner.stretchBackendParam)
        xml.setAttribute("stretchBackend", static_cast<int>(owner.stretchBackendParam->load(std::memory_order_acquire)));
    if (owner.sceneModeParam)
        xml.setAttribute("sceneMode", owner.sceneModeParam->load(std::memory_order_acquire) > 0.5f);
    if (owner.transientOnsetMethodParam)
        xml.setAttribute("transientOnsetMethod", static_cast<int>(owner.transientOnsetMethodParam->load(std::memory_order_acquire)));
    if (owner.transientSensitivityParam)
        xml.setAttribute("transientSensitivity", static_cast<int>(owner.transientSensitivityParam->load(std::memory_order_acquire)));
    if (owner.transientSnapParam)
        xml.setAttribute("transientSnap", static_cast<int>(owner.transientSnapParam->load(std::memory_order_acquire)));
    if (owner.transientSpacingParam)
        xml.setAttribute("transientSpacing", static_cast<int>(owner.transientSpacingParam->load(std::memory_order_acquire)));
    if (owner.sceneRecallModeParam)
        xml.setAttribute("sceneRecallMode", static_cast<int>(owner.getSceneRecallMode()));
    xml.setAttribute("sceneEditorGridEnabled", owner.sceneEditorGridEnabledState);
    xml.setAttribute("sceneEditorGridDivision", owner.sceneEditorGridDivisionState);
    xml.setAttribute("sceneEditorDrawMode", owner.sceneEditorDrawModeEnabledState);
    xml.setAttribute("sceneEditorLaneOverlays", owner.sceneEditorLaneOverlaysEnabledState);
    xml.setAttribute("sceneEditorZoomFactor", owner.sceneEditorZoomFactorState);
    xml.setAttribute("sceneEditorFollowPlayhead", owner.sceneEditorFollowPlayheadState);
    xml.setAttribute("sceneModPageMode", static_cast<int>(owner.sceneModPageModeState));
    xml.setAttribute("sceneTransitionEndSampleDir", owner.getSceneTransitionEndSampleDirectory().getFullPathName());
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        xml.setAttribute("sceneStripAutomationExpanded" + juce::String(stripIndex),
                         owner.sceneEditorStripAutomationExpanded[static_cast<size_t>(stripIndex)]);
        xml.setAttribute("sceneStripHeightExpanded" + juce::String(stripIndex),
                         owner.sceneEditorStripHeightExpanded[static_cast<size_t>(stripIndex)]);
    }
    if (owner.soundTouchEnabledParam)
        xml.setAttribute("soundTouchEnabled", owner.getStretchBackend() != TimeStretchBackend::Resample);
    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
    {
        const auto target = owner.getMacroTarget(i);
        xml.setAttribute("macroCc" + juce::String(i), owner.getMacroMidiCc(i));
        xml.setAttribute("macroTarget" + juce::String(i), legacyMacroRawFromPerformanceTarget(target));
        xml.setAttribute("macroTargetKey" + juce::String(i), performanceTargetKey(target));
    }
    xml.setAttribute("rootNoteMidi", owner.globalRootNoteMidi.load(std::memory_order_acquire));
    xml.setAttribute("pitchScale", owner.globalPitchScale.load(std::memory_order_acquire));
    if (owner.gestureCoordinator != nullptr)
    {
        owner.gestureCoordinator->appendProfilesToXml(xml);
        owner.gestureCoordinator->appendComboProfilesToXml(xml);
    }
    saveGlobalSettingsXml(xml);
}
