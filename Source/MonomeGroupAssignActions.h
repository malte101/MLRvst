#pragma once

class ModernAudioEngine;
class EnhancedAudioStrip;

namespace MonomeGroupAssignActions
{
bool handleButtonPress(ModernAudioEngine& audioEngine, int stripIndex, int x);
void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][16]);
} // namespace MonomeGroupAssignActions
