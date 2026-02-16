#pragma once

class EnhancedAudioStrip;

namespace MonomeFilterActions
{
// subPage: 0 = Frequency, 1 = Resonance, 2 = Type
void handleButtonPress(EnhancedAudioStrip& strip, int x, int subPage);
void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][8], int subPage);
} // namespace MonomeFilterActions

