#include "MonomeGroupAssignActions.h"
#include "AudioEngine.h"

namespace MonomeGroupAssignActions
{
bool handleButtonPress(ModernAudioEngine& audioEngine, int stripIndex, int x)
{
    if (x < 0 || x > 4)
        return false;

    if (x == 0)
        audioEngine.assignStripToGroup(stripIndex, -1);
    else
        audioEngine.assignStripToGroup(stripIndex, x - 1);

    return true;
}

void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][8])
{
    int currentGroup = strip.getGroup();

    newLedState[0][y] = (currentGroup == -1) ? 15 : 4;

    for (int g = 0; g < 4; ++g)
    {
        int buttonX = g + 1;
        newLedState[buttonX][y] = (currentGroup == g) ? 15 : 4;
    }

    for (int x = 5; x < 16; ++x)
        newLedState[x][y] = 0;
}
} // namespace MonomeGroupAssignActions
