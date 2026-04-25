/*
  ==============================================================================

    GestureCoordinator.h
    Gesture profile/combo state, preview, and persistence coordinator

  ==============================================================================
*/

#pragma once

#include "AudioEngine.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class GestureCoordinator
{
public:
    using UserChangeCallback = std::function<void()>;

    explicit GestureCoordinator(UserChangeCallback onUserChange = {});

    void setUserChangeCallback(UserChangeCallback onUserChange);
    void attachAudioEngine(ModernAudioEngine* engine);
    void syncToAudioEngine();

    GestureProfileState getProfileState(GestureProfileId profileId) const;
    float getProfileStepValue(GestureProfileId profileId, int laneIndex, int stepIndex) const;
    void setProfileStepValue(GestureProfileId profileId, int laneIndex, int stepIndex, float value);
    void resetProfile(GestureProfileId profileId);
    void resetAllProfiles();

    GestureComboProfileState getComboProfileState(GestureComboKind kind, int buttonCount, int comboIndex) const;
    float getComboProfileStepValue(GestureComboKind kind,
                                   int buttonCount,
                                   int comboIndex,
                                   int laneIndex,
                                   int stepIndex) const;
    void setComboProfileStepValue(GestureComboKind kind,
                                  int buttonCount,
                                  int comboIndex,
                                  int laneIndex,
                                  int stepIndex,
                                  float value);
    void materializeComboDisplayState(GestureComboKind kind, int buttonCount, int comboIndex);
    void resetComboProfile(GestureComboKind kind, int buttonCount, int comboIndex);
    void resetComboButtonCount(GestureComboKind kind, int buttonCount);
    void resetAllComboProfiles();

    float sampleProfileLane(GestureProfileId profileId, int laneIndex, float phase) const;
    float sampleComboLane(GestureComboKind kind, int comboFlatIndex, int laneIndex, float phase) const;

    bool restoreProfilesFromXml(const juce::XmlElement& xml);
    void appendProfilesToXml(juce::XmlElement& xml) const;
    bool restoreComboProfilesFromXml(const juce::XmlElement& xml);
    void appendComboProfilesToXml(juce::XmlElement& xml) const;
    void expandLegacyProfilesToCombos();

    std::shared_ptr<GestureComboProfileStore> getComboProfileStore() const noexcept;

private:
    enum class OverrideMode : uint8_t
    {
        Inherited = 0,
        Exact,
        Flat
    };

    OverrideMode getOverrideMode(GestureComboKind kind, int comboFlatIndex) const noexcept;
    void setOverrideMode(GestureComboKind kind, int comboFlatIndex, OverrideMode mode) noexcept;
    void clearOverrideModes() noexcept;
    void notifyUserChange();
    void seedComboFromLegacy(GestureComboKind kind, int buttonCount, int comboIndex);

    UserChangeCallback onUserChange;
    ModernAudioEngine* audioEngine = nullptr;
    std::array<std::atomic<float>, kGestureProfileValueCount> gestureProfileValues{};
    std::shared_ptr<GestureComboProfileStore> gestureComboProfileStore;
    std::array<OverrideMode, kStutterGestureComboCount> stutterOverrideModes{};
    std::array<OverrideMode, kScratchGestureComboCount> scratchOverrideModes{};
};
