/*
  ==============================================================================

    StepSequencerDisplay.h
    Visual display for step sequencer mode
    Shows full pattern grid (16/32/48/64 steps)

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

class StepSequencerDisplay : public juce::Component, public juce::Timer
{
public:
    StepSequencerDisplay()
    {
        startTimer(50);  // 20 FPS update rate
    }
    
    ~StepSequencerDisplay() override
    {
        stopTimer();
    }
    
    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        const auto content = bounds.reduced(1).toFloat();

        // Background
        g.fillAll(juce::Colour(0xff1f1f1f));

        const int numSteps = juce::jlimit(16, 64, totalSteps);
        const int numRows = juce::jmax(1, (numSteps + 15) / 16);
        const int stepsPerRow = 16;
        const float stepWidth = content.getWidth() / static_cast<float>(stepsPerRow);
        const float stepHeight = content.getHeight() / static_cast<float>(numRows);
        
        for (int i = 0; i < numSteps; ++i)
        {
            const int row = i / stepsPerRow;
            const int col = i % stepsPerRow;
            juce::Rectangle<float> stepRect(
                content.getX() + (col * stepWidth),
                content.getY() + (row * stepHeight),
                stepWidth - 2.0f,  // 2px gap between steps
                stepHeight - 2.0f
            );
            
            // Determine step color using strip color
            juce::Colour stepColor;
            
            if (i == currentStep && isPlaying)
            {
                // Current step gets an Ableton-style warm highlight.
                stepColor = juce::Colour(0xfff29a36);
            }
            else if (stepPattern[static_cast<size_t>(i)])
            {
                // Active step - strip color
                stepColor = stripColor.withMultipliedSaturation(0.8f).withMultipliedBrightness(0.9f);
            }
            else
            {
                // Inactive step - very dark strip color
                stepColor = juce::Colour(0xff2a2a2a);
            }
            
            // Draw step
            g.setColour(stepColor);
            g.fillRect(stepRect);
            
            // Draw border
            g.setColour(juce::Colour(0xff141414));
            g.drawRect(stepRect, 1.0f);
            
            // Draw step number
            g.setColour(juce::Colour(0xffa8a8a8));
            g.setFont(stepHeight < 18.0f ? 8.0f : 10.0f);
            g.drawText(juce::String(i + 1),
                      stepRect.toNearestInt(), 
                      juce::Justification::centred);
        }
        
        // Draw beat divisions (every 4 steps = 1 beat), per row.
        g.setColour(juce::Colour(0xff4f4f4f));
        for (int col = 4; col < stepsPerRow; col += 4)
        {
            const float x = content.getX() + (col * stepWidth);
            g.drawLine(x, content.getY(), x, content.getBottom(), 1.5f);
        }

        // Draw row separators for multi-row patterns.
        if (numRows > 1)
        {
            g.setColour(juce::Colour(0xff1a1a1a));
            for (int row = 1; row < numRows; ++row)
            {
                const float y = content.getY() + (row * stepHeight);
                g.drawLine(content.getX(), y, content.getRight(), y, 1.0f);
            }
        }
        
        // Draw playback position indicator (independent of step)
        if (playbackPosition >= 0.0f && playbackPosition <= 1.0f)
        {
            float playheadX = content.getX() + (playbackPosition * content.getWidth());
            
            // Vertical line
            g.setColour(juce::Colour(0xffffb347).withAlpha(0.9f));
            g.drawLine(playheadX, content.getY(), playheadX, content.getY() + stepHeight, 2.0f);
            
            // Small triangle at top
            juce::Path triangle;
            triangle.addTriangle(playheadX - 5, 0, playheadX + 5, 0, playheadX, 8);
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(triangle);
        }
    }
    
    void setStepPattern(const std::array<bool, 64>& pattern, int steps)
    {
        stepPattern = pattern;
        totalSteps = juce::jlimit(16, 64, steps);
        repaint();
    }
    
    void setCurrentStep(int step)
    {
        if (currentStep != step)
        {
            currentStep = step;
            repaint();
        }
    }
    
    void setPlaying(bool playing)
    {
        if (isPlaying != playing)
        {
            isPlaying = playing;
            repaint();
        }
    }
    
    void setStripColor(juce::Colour color)
    {
        stripColor = color;
        repaint();
    }
    
    void setPlaybackPosition(float position)
    {
        playbackPosition = position;
        repaint();
    }
    
    void timerCallback() override
    {
        // Regular updates to catch state changes
        repaint();
    }
    
    void mouseDown(const juce::MouseEvent& event) override
    {
        // Allow clicking to toggle steps
        const int stepIndex = getStepIndexFromPosition(event.position);
        if (stepIndex >= 0 && stepIndex < totalSteps)
        {
            if (onStepClicked)
                onStepClicked(stepIndex);
        }
    }
    
    std::function<void(int)> onStepClicked;
    
private:
    std::array<bool, 64> stepPattern = {};
    int totalSteps = 16;
    int currentStep = 0;
    bool isPlaying = false;
    float playbackPosition = -1.0f;  // -1 = hidden, 0.0-1.0 = normalized position
    juce::Colour stripColor = juce::Colour(0xff6f93c8);  // Default muted blue
    
    int getStepIndexFromPosition(juce::Point<float> position) const
    {
        const auto content = getLocalBounds().reduced(1).toFloat();
        const int stepsPerRow = 16;
        const int numRows = juce::jmax(1, (totalSteps + 15) / 16);
        const float stepWidth = content.getWidth() / static_cast<float>(stepsPerRow);
        const float stepHeight = content.getHeight() / static_cast<float>(numRows);
        const float px = juce::jlimit(content.getX(), content.getRight() - 0.001f, position.x) - content.getX();
        const float py = juce::jlimit(content.getY(), content.getBottom() - 0.001f, position.y) - content.getY();
        const int col = juce::jlimit(0, stepsPerRow - 1, static_cast<int>(px / stepWidth));
        const int row = juce::jlimit(0, numRows - 1, static_cast<int>(py / stepHeight));
        const int index = row * stepsPerRow + col;
        return juce::jlimit(0, totalSteps - 1, index);
    }
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepSequencerDisplay)
};
