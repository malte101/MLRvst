/*
  ==============================================================================

    MacroTargetDispatcher.h
    Macro target metadata/read/apply helpers

  ==============================================================================
*/

#pragma once

#include "PerformanceTargets.h"

class MlrVSTAudioProcessor;
class EnhancedAudioStrip;

class MacroTargetDispatcher
{
public:
    static PerformanceTarget getDefaultMacroTarget(int macroIndex);
    static float getDefaultMacroNormalizedValue(PerformanceTarget target);

    static float getNormalizedValueForTarget(const MlrVSTAudioProcessor& processor,
                                             int stripIndex,
                                             const EnhancedAudioStrip& strip,
                                             PerformanceTarget target);

    static void applyTargetValue(MlrVSTAudioProcessor& processor,
                                 int stripIndex,
                                 EnhancedAudioStrip& strip,
                                 PerformanceTarget target,
                                 float normalizedValue);
};
