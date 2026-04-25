/*
  ==============================================================================

    PluginProcessorBrowserPaths.cpp
    Browser/default-path implementation split from PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "GlobalSettingsStore.h"

namespace
{
juce::File restoreStoredAbsolutePath(const juce::String& rawPath)
{
    const auto path = rawPath.trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return {};
    return juce::File(path);
}

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

bool safeFileIsDirectory(const juce::File& file)
{
    return canSafelyProbeFilesystemPath(file) && file.exists() && file.isDirectory();
}
} // namespace

juce::File MlrVSTAudioProcessor::getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    switch (mode)
    {
        case SamplePathMode::Step: return defaultStepDirectories[idx];
        case SamplePathMode::Flip: return defaultFlipDirectories[idx];
        case SamplePathMode::Loop:
        default: return defaultLoopDirectories[idx];
    }
}

MlrVSTAudioProcessor::SamplePathMode MlrVSTAudioProcessor::getSamplePathModeForStrip(int stripIndex) const
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return SamplePathMode::Loop;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            return SamplePathMode::Step;
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            return SamplePathMode::Flip;
    }

    return SamplePathMode::Loop;
}

juce::File MlrVSTAudioProcessor::getRecentSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    switch (mode)
    {
        case SamplePathMode::Step: return recentStepDirectories[idx];
        case SamplePathMode::Flip: return recentFlipDirectories[idx];
        case SamplePathMode::Loop:
        default: return recentLoopDirectories[idx];
    }
}

void MlrVSTAudioProcessor::setRecentSampleDirectory(int stripIndex,
                                                    SamplePathMode mode,
                                                    const juce::File& directory,
                                                    bool persist)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    juce::File normalizedDirectory;
    const auto rawPath = directory.getFullPathName().trim();
    if (directory != juce::File()
        && rawPath.isNotEmpty()
        && juce::File::isAbsolutePath(rawPath))
    {
        normalizedDirectory = directory;
    }

    switch (mode)
    {
        case SamplePathMode::Step:
            recentStepDirectories[idx] = normalizedDirectory;
            break;
        case SamplePathMode::Flip:
            recentFlipDirectories[idx] = normalizedDirectory;
            break;
        case SamplePathMode::Loop:
        default:
            recentLoopDirectories[idx] = normalizedDirectory;
            break;
    }

    if (safeFileIsDirectory(normalizedDirectory))
        lastSampleFolder = normalizedDirectory;

    if (persist)
        GlobalSettingsStore::saveDefaultPaths(*this);
}

void MlrVSTAudioProcessor::rememberLoadedSamplePathForStrip(int stripIndex, const juce::File& file, bool persist)
{
    rememberLoadedSamplePathForStripMode(stripIndex, file, getSamplePathModeForStrip(stripIndex), persist);
}

void MlrVSTAudioProcessor::rememberLoadedSamplePathForStripMode(int stripIndex,
                                                                const juce::File& file,
                                                                SamplePathMode mode,
                                                                bool persist)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    currentStripFiles[idx] = file;

    const auto directory = file.getParentDirectory();
    if (directory != juce::File())
        setRecentSampleDirectory(stripIndex, mode, directory, false);

    if (persist)
        GlobalSettingsStore::saveDefaultPaths(*this);
}

juce::File MlrVSTAudioProcessor::getCurrentBrowserDirectoryForStrip(int stripIndex) const
{
    return getCurrentBrowserDirectoryForStrip(stripIndex, getSamplePathModeForStrip(stripIndex));
}

juce::File MlrVSTAudioProcessor::getCurrentBrowserDirectoryForStrip(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto isValidDir = [](const juce::File& dir)
    {
        return safeFileIsDirectory(dir);
    };

    const auto recentDir = getRecentSampleDirectory(stripIndex, mode);
    if (isValidDir(recentDir))
        return recentDir;

    const auto currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    const auto currentDir = currentFile.getParentDirectory();
    if (isValidDir(currentDir))
        return currentDir;

    const auto selectedDir = getDefaultSampleDirectory(stripIndex, mode);
    if (isValidDir(selectedDir))
        return selectedDir;

    const std::array<SamplePathMode, 3> fallbackModes {
        SamplePathMode::Flip,
        SamplePathMode::Step,
        SamplePathMode::Loop
    };
    for (const auto fallbackMode : fallbackModes)
    {
        if (fallbackMode == mode)
            continue;
        const auto fallbackDefaultDir = getDefaultSampleDirectory(stripIndex, fallbackMode);
        if (isValidDir(fallbackDefaultDir))
            return fallbackDefaultDir;
        const auto fallbackRecentDir = getRecentSampleDirectory(stripIndex, fallbackMode);
        if (isValidDir(fallbackRecentDir))
            return fallbackRecentDir;
    }

    const auto& favoriteSet = (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories
        : browserFavoriteDirectories;
    for (const auto& favoriteDir : favoriteSet)
    {
        if (isValidDir(favoriteDir))
            return favoriteDir;
    }

    if (isValidDir(lastSampleFolder))
        return lastSampleFolder;

    for (int i = 0; i < MaxStrips; ++i)
    {
        if (i == stripIndex)
            continue;

        if (isValidDir(defaultLoopDirectories[static_cast<size_t>(i)]))
            return defaultLoopDirectories[static_cast<size_t>(i)];
        if (isValidDir(defaultStepDirectories[static_cast<size_t>(i)]))
            return defaultStepDirectories[static_cast<size_t>(i)];
        if (isValidDir(defaultFlipDirectories[static_cast<size_t>(i)]))
            return defaultFlipDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentLoopDirectories[static_cast<size_t>(i)]))
            return recentLoopDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentStepDirectories[static_cast<size_t>(i)]))
            return recentStepDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentFlipDirectories[static_cast<size_t>(i)]))
            return recentFlipDirectories[static_cast<size_t>(i)];

        const auto otherCurrentDir = currentStripFiles[static_cast<size_t>(i)].getParentDirectory();
        if (isValidDir(otherCurrentDir))
            return otherCurrentDir;
    }

    const auto homeDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (isValidDir(homeDir))
        return homeDir;

    return {};
}

juce::String MlrVSTAudioProcessor::getStripDisplaySampleName(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto& pendingFile = pendingLoopStripFiles[static_cast<size_t>(stripIndex)];
    if (isLoopStripLoadInFlight(stripIndex) && pendingFile.getFullPathName().isNotEmpty())
        return pendingFile.getFileNameWithoutExtension();

    const auto& currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    if (currentFile.getFullPathName().isNotEmpty())
        return currentFile.getFileNameWithoutExtension();

    if (auto* engine = getSampleModeEngine(stripIndex, false))
    {
        const auto snapshot = engine->getStateSnapshot();
        if (snapshot.displayName.isNotEmpty())
            return snapshot.displayName;
    }

    return {};
}

juce::File MlrVSTAudioProcessor::getBrowserFavoriteDirectory(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return {};

    const auto mode = getSamplePathModeForStrip(stripIndex);
    return (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories[static_cast<size_t>(slot)]
        : browserFavoriteDirectories[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoritePadHeld(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return browserFavoritePadHeld[static_cast<size_t>(stripIndex)][static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteSaveBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteSaveBurstUntilMs[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteMissingBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteMissingBurstUntilMs[static_cast<size_t>(slot)];
}

void MlrVSTAudioProcessor::beginBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    browserFavoritePadHeld[stripIdx][slotIdx] = true;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
    browserFavoritePadPressStartMs[stripIdx][slotIdx] = juce::Time::getMillisecondCounter();
}

void MlrVSTAudioProcessor::endBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    const bool wasHeld = browserFavoritePadHeld[stripIdx][slotIdx];
    const bool holdSaveTriggered = browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx];

    if (wasHeld && !holdSaveTriggered)
    {
        if (!recallBrowserFavoriteDirectoryForStrip(stripIndex, slot))
            browserFavoriteMissingBurstUntilMs[slotIdx] = juce::Time::getMillisecondCounter() + browserFavoriteMissingBurstDurationMs;
    }

    browserFavoritePadHeld[stripIdx][slotIdx] = false;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
}

void MlrVSTAudioProcessor::setDefaultSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);

    if (directory == juce::File())
    {
        if (mode == SamplePathMode::Step)
            defaultStepDirectories[idx] = juce::File();
        else if (mode == SamplePathMode::Flip)
            defaultFlipDirectories[idx] = juce::File();
        else
            defaultLoopDirectories[idx] = juce::File();
        GlobalSettingsStore::saveDefaultPaths(*this);
        return;
    }

    if (!safeFileIsDirectory(directory))
        return;

    if (mode == SamplePathMode::Step)
        defaultStepDirectories[idx] = directory;
    else if (mode == SamplePathMode::Flip)
        defaultFlipDirectories[idx] = directory;
    else
        defaultLoopDirectories[idx] = directory;

    GlobalSettingsStore::saveDefaultPaths(*this);
}

void MlrVSTAudioProcessor::setCurrentBrowserDirectoryForStrip(int stripIndex,
                                                              SamplePathMode mode,
                                                              const juce::File& directory)
{
    setRecentSampleDirectory(stripIndex, mode, directory, true);
}

bool MlrVSTAudioProcessor::saveBrowserFavoriteDirectoryFromStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto directory = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!safeFileIsDirectory(directory))
        return false;

    const auto mode = getSamplePathModeForStrip(stripIndex);
    if (mode == SamplePathMode::Flip)
        browserFlipFavoriteDirectories[static_cast<size_t>(slot)] = directory;
    else
        browserFavoriteDirectories[static_cast<size_t>(slot)] = directory;
    GlobalSettingsStore::saveDefaultPaths(*this);
    return true;
}

bool MlrVSTAudioProcessor::recallBrowserFavoriteDirectoryForStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto slotIdx = static_cast<size_t>(slot);
    const auto mode = getSamplePathModeForStrip(stripIndex);
    const auto directory = (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories[slotIdx]
        : browserFavoriteDirectories[slotIdx];
    if (!safeFileIsDirectory(directory))
    {
        if (mode == SamplePathMode::Flip)
            browserFlipFavoriteDirectories[slotIdx] = juce::File();
        else
            browserFavoriteDirectories[slotIdx] = juce::File();
        GlobalSettingsStore::saveDefaultPaths(*this);
        return false;
    }

    setRecentSampleDirectory(stripIndex, mode, directory);
    return true;
}

bool MlrVSTAudioProcessor::isAudioFileSupported(const juce::File& file) const
{
    if (!safeFileExistsAsFile(file))
        return false;

    return file.hasFileExtension(".wav")
        || file.hasFileExtension(".aif")
        || file.hasFileExtension(".aiff")
        || file.hasFileExtension(".mp3")
        || file.hasFileExtension(".ogg")
        || file.hasFileExtension(".flac");
}

void MlrVSTAudioProcessor::appendDefaultPathsToState(juce::ValueTree& state) const
{
    auto paths = state.getOrCreateChildWithName("DefaultPaths", nullptr);
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);
        const auto flipKey = "flipDir" + juce::String(i);
        const auto recentLoopKey = "recentLoopDir" + juce::String(i);
        const auto recentStepKey = "recentStepDir" + juce::String(i);
        const auto recentFlipKey = "recentFlipDir" + juce::String(i);
        paths.setProperty(loopKey, defaultLoopDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(stepKey, defaultStepDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(flipKey, defaultFlipDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentLoopKey, recentLoopDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentStepKey, recentStepDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentFlipKey, recentFlipDirectories[idx].getFullPathName(), nullptr);
    }

    paths.setProperty("lastSampleFolder", lastSampleFolder.getFullPathName(), nullptr);

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        const auto flipKey = "favoriteFlipDir" + juce::String(slot);
        paths.setProperty(key, browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName(), nullptr);
        paths.setProperty(flipKey, browserFlipFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName(), nullptr);
    }
}

void MlrVSTAudioProcessor::loadDefaultPathsFromState(const juce::ValueTree& state)
{
    auto paths = state.getChildWithName("DefaultPaths");
    if (!paths.isValid())
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);
        const auto flipKey = "flipDir" + juce::String(i);
        const auto recentLoopKey = "recentLoopDir" + juce::String(i);
        const auto recentStepKey = "recentStepDir" + juce::String(i);
        const auto recentFlipKey = "recentFlipDir" + juce::String(i);

        defaultLoopDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(loopKey).toString());
        defaultStepDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(stepKey).toString());
        defaultFlipDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(flipKey).toString());
        recentLoopDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentLoopKey).toString());
        recentStepDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentStepKey).toString());
        recentFlipDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentFlipKey).toString());
    }

    lastSampleFolder = restoreStoredAbsolutePath(paths.getProperty("lastSampleFolder").toString());

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        const auto flipKey = "favoriteFlipDir" + juce::String(slot);
        browserFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(paths.getProperty(key).toString());
        browserFlipFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(paths.getProperty(flipKey).toString());
    }

    GlobalSettingsStore::saveDefaultPaths(*this);
}

void MlrVSTAudioProcessor::resetCurrentBrowserDirectoriesToDefaultPaths(bool persist)
{
    juce::File firstStoredDefault;
    auto copyDefaultDirectory = [&firstStoredDefault](const juce::File& directory) -> juce::File
    {
        const auto restored = restoreStoredAbsolutePath(directory.getFullPathName());
        if (restored == juce::File())
            return {};
        if (firstStoredDefault == juce::File())
            firstStoredDefault = restored;
        return restored;
    };

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        recentLoopDirectories[idx] = copyDefaultDirectory(defaultLoopDirectories[idx]);
        recentStepDirectories[idx] = copyDefaultDirectory(defaultStepDirectories[idx]);
        recentFlipDirectories[idx] = copyDefaultDirectory(defaultFlipDirectories[idx]);
    }

    lastSampleFolder = firstStoredDefault;

    if (persist)
        GlobalSettingsStore::saveDefaultPaths(*this);
}

void MlrVSTAudioProcessor::loadAdjacentFile(int stripIndex, int direction)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const bool hasCurrentAudio = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
        ? hasSampleModeAudio(stripIndex)
        : strip->hasAudio();
    juce::File currentFile;
    if (isLoopStripLoadInFlight(stripIndex))
        currentFile = pendingLoopStripFiles[static_cast<size_t>(stripIndex)];
    else if (hasCurrentAudio)
        currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];

    juce::File folderToUse = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!safeFileIsDirectory(folderToUse))
        return;

    juce::Array<juce::File> audioFiles;
    for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, false))
    {
        if (isAudioFileSupported(file))
            audioFiles.add(file);
    }

    if (audioFiles.size() == 0)
    {
        for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, true))
        {
            if (isAudioFileSupported(file))
                audioFiles.add(file);
        }
    }

    if (audioFiles.size() == 0)
        return;
    audioFiles.sort();

    int currentIndex = -1;
    if (safeFileExistsAsFile(currentFile))
    {
        for (int i = 0; i < audioFiles.size(); ++i)
        {
            if (audioFiles[i] == currentFile)
            {
                currentIndex = i;
                break;
            }
        }
    }

    juce::File fileToLoad;
    if (currentIndex < 0)
    {
        fileToLoad = audioFiles[0];
    }
    else
    {
        int newIndex = currentIndex + direction;
        if (newIndex < 0)
            newIndex = audioFiles.size() - 1;
        if (newIndex >= audioFiles.size())
            newIndex = 0;
        fileToLoad = audioFiles[newIndex];
    }

    if (!safeFileExistsAsFile(fileToLoad))
        return;

    loadSampleToStripPreservingPlaybackState(stripIndex, fileToLoad);
}
