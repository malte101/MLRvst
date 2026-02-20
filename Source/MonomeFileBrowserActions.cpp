#include "MonomeFileBrowserActions.h"
#include "PluginProcessor.h"

namespace MonomeFileBrowserActions
{
void handleButtonPress(MlrVSTAudioProcessor& processor, EnhancedAudioStrip& strip, int stripIndex, int x)
{
    if (x == 0)
        processor.loadAdjacentFile(stripIndex, -1);  // Prev
    else if (x == 1)
        processor.loadAdjacentFile(stripIndex, 1);   // Next
    else if (x >= 11 && x <= 14)
    {
        // Button 11=1, 12=2, 13=4, 14=8 bars
        int bars = 1;
        if (x == 12) bars = 2;
        else if (x == 13) bars = 4;
        else if (x == 14) bars = 8;

        strip.setRecordingBars(bars);

        if (strip.hasAudio() && !strip.isPlaying())
        {
            strip.setBeatsPerLoop(static_cast<float>(bars * 4));
            processor.setPendingBarLengthApply(stripIndex, false);
        }
        else if (strip.hasAudio())
        {
            processor.setPendingBarLengthApply(stripIndex, true);
        }
    }
    else if (x == 15)
    {
        processor.captureRecentAudioToStrip(stripIndex);
    }
}

void renderRow(const ModernAudioEngine& engine, const EnhancedAudioStrip& strip, int y, int newLedState[16][8])
{
    // File browser controls (always visible)
    newLedState[0][y] = 8;  // Prev
    newLedState[1][y] = 8;  // Next

    const bool isArmed = !strip.hasAudio();

    double beatPos = engine.getCurrentBeat();
    if (!std::isfinite(beatPos) || beatPos < 0.0)
        beatPos = 0.0;

    const double beatFraction = beatPos - std::floor(beatPos);

    // Double-speed smooth pulse for armed state and record button
    double doubleBeatFraction = beatFraction * 2.0;
    doubleBeatFraction = doubleBeatFraction - std::floor(doubleBeatFraction);
    double smoothPulse = (std::sin((doubleBeatFraction - 0.5) * 2.0 * 3.14159) + 1.0) / 2.0;
    smoothPulse = juce::jlimit(0.0, 1.0, smoothPulse);

    // Buttons 11-14: Loop length selector (1, 2, 4, 8 bars)
    const int selectedBars = strip.getRecordingBars();

    for (int buttonX = 11; buttonX <= 14; ++buttonX)
    {
        int bars = 1;
        if (buttonX == 12) bars = 2;
        else if (buttonX == 13) bars = 4;
        else if (buttonX == 14) bars = 8;

        if (bars == selectedBars)
        {
            double blinkPhase = 0.0;
            if (bars == 1)
                blinkPhase = doubleBeatFraction;  // 2x speed
            else if (bars == 2)
                blinkPhase = beatFraction;        // 1x speed
            else if (bars == 4)
                blinkPhase = (beatPos / 4.0) - std::floor(beatPos / 4.0);
            else
                blinkPhase = (beatPos / 8.0) - std::floor(beatPos / 8.0);

            double lengthPulse = (std::sin((blinkPhase - 0.5) * 2.0 * 3.14159) + 1.0) / 2.0;
            lengthPulse = juce::jlimit(0.0, 1.0, lengthPulse);

            if (isArmed)
                newLedState[buttonX][y] = 10 + static_cast<int>(lengthPulse * 5.0);
            else
                newLedState[buttonX][y] = 6 + static_cast<int>(lengthPulse * 6.0);
        }
        else
        {
            newLedState[buttonX][y] = 3;
        }
    }

    // Button 15: Record button pulse
    if (isArmed)
        newLedState[15][y] = 10 + static_cast<int>(smoothPulse * 5.0);
    else
        newLedState[15][y] = 8 + static_cast<int>(smoothPulse * 5.0);
}
} // namespace MonomeFileBrowserActions
