#pragma once

class MlrVSTAudioProcessor;
class ModernAudioEngine;
class EnhancedAudioStrip;

namespace MonomeFileBrowserActions
{
void handleButtonPress(MlrVSTAudioProcessor& processor, EnhancedAudioStrip& strip, int stripIndex, int x);
void handleButtonRelease(MlrVSTAudioProcessor& processor, EnhancedAudioStrip& strip, int stripIndex, int x);
void renderRow(const MlrVSTAudioProcessor& processor, const ModernAudioEngine& engine, const EnhancedAudioStrip& strip, int stripIndex, int y, int newLedState[16][16]);
} // namespace MonomeFileBrowserActions
