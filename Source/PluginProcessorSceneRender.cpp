/*
  ==============================================================================

    PluginProcessorSceneRender.cpp
    Extracted active scene rendering helpers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include <array>

namespace
{
MlrVSTAudioProcessor::SceneChainTransitionType sanitizeRuntimeSceneChainTransitionType(int rawType)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return),
        rawType));
}

MlrVSTAudioProcessor::SceneChainTransitionScope sanitizeRuntimeSceneChainTransitionScope(int rawScope)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionScope>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Flip),
        rawScope));
}

bool sceneTransitionScopeMatchesStrip(MlrVSTAudioProcessor::SceneChainTransitionScope scope,
                                      const EnhancedAudioStrip& strip)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    using PlayMode = EnhancedAudioStrip::PlayMode;

    switch (sanitizeRuntimeSceneChainTransitionScope(static_cast<int>(scope)))
    {
        case Scope::Loops:
        {
            const auto playMode = strip.getPlayMode();
            return playMode == PlayMode::Loop
                || playMode == PlayMode::Gate
                || playMode == PlayMode::OneShot;
        }
        case Scope::Steps:
            return strip.getPlayMode() == PlayMode::Step;
        case Scope::Grains:
            return strip.getPlayMode() == PlayMode::Grain;
        case Scope::Flip:
            return strip.getPlayMode() == PlayMode::Sample;
        case Scope::All:
        default:
            return true;
    }
}

bool sceneTransitionStripHasPlayableContent(const EnhancedAudioStrip& strip)
{
    const bool stepMode = strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
    const bool sampleMode = strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Sample;
    const auto* stepSampler = stepMode ? strip.getStepSampler() : nullptr;
    return sampleMode || strip.hasAudio() || (stepSampler != nullptr && stepSampler->getHasAudio());
}

MlrVSTAudioProcessor::SceneChainTransitionScope sceneTransitionEffectiveRenderScope(
    MlrVSTAudioProcessor::SceneChainTransitionScope requestedScope,
    ModernAudioEngine& engine)
{
    const auto safeScope = sanitizeRuntimeSceneChainTransitionScope(static_cast<int>(requestedScope));
    if (safeScope == MlrVSTAudioProcessor::SceneChainTransitionScope::All)
        return safeScope;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        const auto* strip = engine.getStrip(stripIndex);
        if (strip != nullptr
            && sceneTransitionStripHasPlayableContent(*strip)
            && sceneTransitionScopeMatchesStrip(safeScope, *strip))
        {
            return safeScope;
        }
    }

    return MlrVSTAudioProcessor::SceneChainTransitionScope::All;
}
} // namespace

void MlrVSTAudioProcessor::renderActiveSceneAudio(juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer& midiMessages,
                                                  const juce::AudioPlayHead::PositionInfo& posInfo,
                                                  bool allowSeparateStripRouting,
                                                  bool applySceneTransitionOverlay)
{
    if (audioEngine == nullptr)
    {
        buffer.clear();
        return;
    }

    if (masterVolumeParam)
        audioEngine->setMasterVolume(*masterVolumeParam);
    if (limiterThresholdParam)
        audioEngine->setLimiterThresholdDb(limiterThresholdParam->load(std::memory_order_acquire));
    if (limiterEnabledParam)
        audioEngine->setLimiterEnabled(limiterEnabledParam->load(std::memory_order_acquire) > 0.5f);
    if (quantizeParam)
    {
        const int quantizeChoice = static_cast<int>(*quantizeParam);
        const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
        const int division = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
        audioEngine->setQuantization(division);
    }
    if (pitchSmoothingParam)
        audioEngine->setPitchSmoothingTime(*pitchSmoothingParam);
    if (inputMonitorParam)
        audioEngine->setInputMonitorVolume(*inputMonitorParam);
    if (crossfadeLengthParam)
        audioEngine->setCrossfadeLengthMs(*crossfadeLengthParam);
    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);
    audioEngine->setGlobalStretchBackend(getStretchBackend());
    audioEngine->setGlobalContinuousTraversal(usesContinuousTraversal());
    syncTransientDetectionSettingsFromParameters(false);
    audioEngine->setGlobalTempoMatchBackend(getLoopTempoMatchBackend());
    if (masterDuckTriggerStripParam)
    {
        const int triggerChoice = juce::jlimit(0,
                                               MaxStrips,
                                               static_cast<int>(masterDuckTriggerStripParam->load(std::memory_order_acquire)));
        audioEngine->setMasterDuckTriggerStrip(triggerChoice - 1);
    }

    applyMomentaryStutterMacro(posInfo);
    processScenePerformancePlayback(posInfo, buffer.getNumSamples());
    updateAudioEngineSceneModulationContext();
    const int64_t blockStartSample = audioEngine->getGlobalSampleCount();
    const auto transitionOverlayType = sanitizeRuntimeSceneChainTransitionType(
        sceneBoundaryTransitionType.load(std::memory_order_acquire));
    const auto requestedTransitionOverlayScope = sanitizeRuntimeSceneChainTransitionScope(
        sceneBoundaryTransitionScope.load(std::memory_order_acquire));
    const auto transitionOverlayScope = sceneTransitionEffectiveRenderScope(requestedTransitionOverlayScope,
                                                                           *audioEngine);
    const bool shouldRestoreTransitionOverlay = applySceneTransitionOverlay
        && transitionOverlayType != SceneChainTransitionType::None
        && transitionOverlayType != SceneChainTransitionType::Return;
    std::array<StripControlState::ResolvedOwnedStripControlState, MaxStrips> transitionOverlayOriginalStates{};
    std::array<bool, MaxStrips> transitionOverlayOriginalStateValid{};
    if (shouldRestoreTransitionOverlay)
    {
        const StripControlState::ParameterView emptyView;
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            auto* strip = audioEngine->getStrip(stripIndex);
            if (strip == nullptr)
                continue;
            if (!sceneTransitionScopeMatchesStrip(transitionOverlayScope, *strip))
                continue;

            if (!sceneTransitionStripHasPlayableContent(*strip))
                continue;

            transitionOverlayOriginalStates[static_cast<size_t>(stripIndex)] =
                StripControlState::resolveFromParameters(emptyView, *strip);
            transitionOverlayOriginalStateValid[static_cast<size_t>(stripIndex)] = true;
        }

        applySceneBoundaryTransitionOverlay(posInfo, buffer.getNumSamples());
    }

    auto restoreSceneTransitionOverlay = [this,
                                          &transitionOverlayOriginalStates,
                                          &transitionOverlayOriginalStateValid]()
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            if (!transitionOverlayOriginalStateValid[static_cast<size_t>(stripIndex)])
                continue;

            auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
            if (strip == nullptr)
                continue;

            const auto& originalState = transitionOverlayOriginalStates[static_cast<size_t>(stripIndex)];
            applyResolvedOwnedStripControlState(*strip, originalState);
            applyResolvedStripFilterState(*strip, originalState);
            applyResolvedStripDelayState(*strip, originalState);
        }
    };

    const bool separateStripRouting = allowSeparateStripRouting
        && (outputRoutingParam != nullptr && *outputRoutingParam > 0.5f)
        && getBusCount(false) > 1;
    if (separateStripRouting)
    {
        std::array<std::array<float*, 2>, MaxStrips> stripBusChannels{};
        std::array<juce::AudioBuffer<float>, MaxStrips> stripBusViews;
        std::array<juce::AudioBuffer<float>*, MaxStrips> stripBusTargets{};
        stripBusTargets.fill(nullptr);

        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const int busIndex = stripIndex;
            if (busIndex >= getBusCount(false))
                continue;

            auto busBuffer = getBusBuffer(buffer, false, busIndex);
            if (busBuffer.getNumChannels() <= 0 || busBuffer.getNumSamples() <= 0)
                continue;

            auto& channelPtrs = stripBusChannels[static_cast<size_t>(stripIndex)];
            channelPtrs.fill(nullptr);
            channelPtrs[0] = busBuffer.getWritePointer(0);
            channelPtrs[1] = busBuffer.getNumChannels() > 1
                ? busBuffer.getWritePointer(1)
                : busBuffer.getWritePointer(0);

            stripBusViews[static_cast<size_t>(stripIndex)].setDataToReferTo(
                channelPtrs.data(), 2, busBuffer.getNumSamples());
            stripBusTargets[static_cast<size_t>(stripIndex)] =
                &stripBusViews[static_cast<size_t>(stripIndex)];
        }

        auto* mainTarget = stripBusTargets[0];
        if (mainTarget == nullptr)
            mainTarget = &buffer;
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            if (stripBusTargets[static_cast<size_t>(stripIndex)] == nullptr)
                stripBusTargets[static_cast<size_t>(stripIndex)] = mainTarget;
        }

        audioEngine->processBlock(buffer, midiMessages, posInfo, &stripBusTargets);
        renderSceneTransitionEndSampleVoices(buffer, posInfo, blockStartSample, true);
        if (shouldRestoreTransitionOverlay)
            restoreSceneTransitionOverlay();
        return;
    }

    audioEngine->processBlock(buffer, midiMessages, posInfo, nullptr);
    renderSceneTransitionEndSampleVoices(buffer, posInfo, blockStartSample, true);
    if (shouldRestoreTransitionOverlay)
        restoreSceneTransitionOverlay();
}

void MlrVSTAudioProcessor::renderActiveSceneAudioRange(juce::AudioBuffer<float>& destination,
                                                       juce::MidiBuffer& midiMessages,
                                                       const juce::AudioPlayHead::PositionInfo& posInfo,
                                                       int startOffset)
{
    if (audioEngine == nullptr || startOffset >= destination.getNumSamples())
        return;

    juce::AudioPlayHead::PositionInfo adjustedPos = posInfo;
    if (startOffset > 0
        && posInfo.getPpqPosition().hasValue()
        && posInfo.getBpm().hasValue()
        && *posInfo.getBpm() > 0.0
        && currentSampleRate > 0.0)
    {
        const double ppqAdvance = (static_cast<double>(startOffset) * *posInfo.getBpm())
            / (60.0 * currentSampleRate);
        adjustedPos.setPpqPosition(*posInfo.getPpqPosition() + ppqAdvance);
    }

    std::array<float*, kSceneRecallBlendMaxChannels> subViewChannels{};
    juce::AudioBuffer<float> subView;
    juce::AudioBuffer<float>* targetBuffer = &destination;
    if (startOffset > 0)
    {
        const int targetChannels = juce::jmin(destination.getNumChannels(), kSceneRecallBlendMaxChannels);
        for (int channel = 0; channel < targetChannels; ++channel)
            subViewChannels[static_cast<size_t>(channel)] = destination.getWritePointer(channel, startOffset);
        subView.setDataToReferTo(subViewChannels.data(), targetChannels, destination.getNumSamples() - startOffset);
        targetBuffer = &subView;
    }

    juce::MidiBuffer incomingMidi;
    renderActiveSceneAudio(*targetBuffer, incomingMidi, adjustedPos, false, false);
    juce::ignoreUnused(midiMessages);
}

MlrVSTAudioProcessor::ScenePlaybackOwner MlrVSTAudioProcessor::getRenderedScenePlaybackOwner() const noexcept
{
    if (activeScenePlaybackHandle.active)
        return activeScenePlaybackHandle.sequenceDriven ? ScenePlaybackOwner::Chain
                                                       : ScenePlaybackOwner::Manual;

    return scenePlaybackOwner;
}

bool MlrVSTAudioProcessor::hasAnyLiveScenePlayback() const noexcept
{
    if (audioEngine == nullptr)
        return false;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr || !strip->isPlaying())
            continue;

        const bool stepMode = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
        const auto* stepSampler = stepMode ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = strip->hasAudio()
            || (stepSampler != nullptr && stepSampler->getHasAudio());
        if (hasPlayableContent)
            return true;
    }

    return false;
}
