#pragma once

class EnhancedAudioStrip;
class MlrVSTAudioProcessor;

namespace MonomeMixActions
{
void applyButtonPressLive(EnhancedAudioStrip& strip,
                          int x,
                          int mode,
                          int gatePageMode);

void handleButtonPress(MlrVSTAudioProcessor& processor,
                       EnhancedAudioStrip& strip,
                       int stripIndex,
                       int x,
                       int mode);

void renderRow(const EnhancedAudioStrip& strip,
               const MlrVSTAudioProcessor& processor,
               int y,
               int newLedState[16][16],
               int mode);

void handleGrainPageButtonPress(EnhancedAudioStrip& targetStrip,
                                int controlRow,
                                int x);
void applyGrainPageButtonPressLive(EnhancedAudioStrip& targetStrip,
                                   int controlRow,
                                   int x);

void renderGrainPageRow(const EnhancedAudioStrip& targetStrip,
                        int controlRow,
                        int y,
                        int newLedState[16][16]);

void handleDelayPageButtonPress(MlrVSTAudioProcessor& processor,
                                EnhancedAudioStrip& targetStrip,
                                int stripIndex,
                                int controlRow,
                                int x);

void renderDelayPageRow(const EnhancedAudioStrip& targetStrip,
                        int controlRow,
                        int y,
                        int newLedState[16][16]);
} // namespace MonomeMixActions
