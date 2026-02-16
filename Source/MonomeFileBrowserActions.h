#pragma once

class MlrVSTAudioProcessor;
class ModernAudioEngine;
class EnhancedAudioStrip;

namespace MonomeFileBrowserActions
{
void handleButtonPress(MlrVSTAudioProcessor& processor, EnhancedAudioStrip& strip, int stripIndex, int x);
void renderRow(const ModernAudioEngine& engine, const EnhancedAudioStrip& strip, int y, int newLedState[16][8]);
} // namespace MonomeFileBrowserActions

