#pragma once

class EnhancedAudioStrip;
class MlrVSTAudioProcessor;

namespace MonomeMixActions
{
void handleButtonPress(MlrVSTAudioProcessor& processor,
                       EnhancedAudioStrip& strip,
                       int stripIndex,
                       int x,
                       int mode);

void renderRow(const EnhancedAudioStrip& strip,
               int y,
               int newLedState[16][8],
               int mode);
} // namespace MonomeMixActions
