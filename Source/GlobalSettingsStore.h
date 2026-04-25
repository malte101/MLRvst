/*
  ==============================================================================

    GlobalSettingsStore.h
    Persistent default-path and global-settings serialization helpers

  ==============================================================================
*/

#pragma once

class MlrVSTAudioProcessor;

class GlobalSettingsStore
{
public:
    static void loadDefaultPaths(MlrVSTAudioProcessor& processor);
    static void saveDefaultPaths(const MlrVSTAudioProcessor& processor);
    static void loadControlPages(MlrVSTAudioProcessor& processor);
    static void saveControlPages(const MlrVSTAudioProcessor& processor);
    static void loadGlobalControls(MlrVSTAudioProcessor& processor);
};
