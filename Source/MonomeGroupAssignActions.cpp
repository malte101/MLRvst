#include "MonomeGroupAssignActions.h"
#include "AudioEngine.h"

namespace MonomeGroupAssignActions
{
bool handleButtonPress(ModernAudioEngine& audioEngine, int stripIndex, int x)
{
    if (x >= 0 && x <= 4)
    {
        if (x == 0)
            audioEngine.assignStripToGroup(stripIndex, -1);
        else
            audioEngine.assignStripToGroup(stripIndex, x - 1);

        return true;
    }

    auto* strip = audioEngine.getStrip(stripIndex);
    if (!strip)
        return false;

    // Middle section: playback direction mode selection.
    // 6=Normal, 7=Reverse, 8=PingPong, 9=Random, 10=RandomWalk, 11=RandomSlice
    if (x >= 6 && x <= 11)
    {
        switch (x - 6)
        {
            case 0: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal); break;
            case 1: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Reverse); break;
            case 2: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::PingPong); break;
            case 3: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Random); break;
            case 4: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::RandomWalk); break;
            case 5: strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::RandomSlice); break;
            default: return false;
        }

        return true;
    }

    // Right section: strip type selection.
    // 13=Loop, 14=Step, 15=Grain
    if (x >= 13 && x <= 15)
    {
        switch (x - 13)
        {
            case 0: strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop); break;
            case 1: strip->setPlayMode(EnhancedAudioStrip::PlayMode::Step); break;
            case 2: strip->setPlayMode(EnhancedAudioStrip::PlayMode::Grain); break;
            default: return false;
        }

        return true;
    }

    return false;
}

void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][16])
{
    int currentGroup = strip.getGroup();

    newLedState[0][y] = (currentGroup == -1) ? 15 : 4;

    for (int g = 0; g < 4; ++g)
    {
        int buttonX = g + 1;
        newLedState[buttonX][y] = (currentGroup == g) ? 15 : 4;
    }

    newLedState[5][y] = 0;   // section spacer between group and direction modes
    newLedState[12][y] = 0;  // section spacer between direction and strip-type modes

    const auto directionMode = strip.getDirectionMode();
    newLedState[6][y] = (directionMode == EnhancedAudioStrip::DirectionMode::Normal) ? 15 : 4;
    newLedState[7][y] = (directionMode == EnhancedAudioStrip::DirectionMode::Reverse) ? 15 : 4;
    newLedState[8][y] = (directionMode == EnhancedAudioStrip::DirectionMode::PingPong) ? 15 : 4;
    newLedState[9][y] = (directionMode == EnhancedAudioStrip::DirectionMode::Random) ? 15 : 4;
    newLedState[10][y] = (directionMode == EnhancedAudioStrip::DirectionMode::RandomWalk) ? 15 : 4;
    newLedState[11][y] = (directionMode == EnhancedAudioStrip::DirectionMode::RandomSlice) ? 15 : 4;

    const auto playMode = strip.getPlayMode();
    newLedState[13][y] = (playMode == EnhancedAudioStrip::PlayMode::Loop) ? 15 : 4;
    newLedState[14][y] = (playMode == EnhancedAudioStrip::PlayMode::Step) ? 15 : 4;
    newLedState[15][y] = (playMode == EnhancedAudioStrip::PlayMode::Grain) ? 15 : 4;
}
} // namespace MonomeGroupAssignActions
