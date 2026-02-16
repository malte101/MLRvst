/*
  ==============================================================================

    StepSequencerDisplay.h
    Visual display for step sequencer mode
    Shows 16-step grid with current step indicator

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
        
        // Background
        g.fillAll(juce::Colour(0xff1f1f1f));
        
        // Calculate step size
        int numSteps = 16;
        float stepWidth = bounds.getWidth() / static_cast<float>(numSteps);
        float stepHeight = bounds.getHeight();
        
        for (int i = 0; i < numSteps; ++i)
        {
            juce::Rectangle<float> stepRect(
                i * stepWidth,
                0.0f,
                stepWidth - 2.0f,  // 2px gap between steps
                stepHeight
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
            g.setFont(10.0f);
            g.drawText(juce::String(i + 1), 
                      stepRect.toNearestInt(), 
                      juce::Justification::centred);
        }
        
        // Draw beat divisions (every 4 steps = 1 beat)
        g.setColour(juce::Colour(0xff4f4f4f));
        for (int i = 4; i < numSteps; i += 4)
        {
            float x = i * stepWidth;
            g.drawLine(x, 0, x, stepHeight, 2.0f);
        }
        
        // Draw playback position indicator (independent of step)
        if (playbackPosition >= 0.0f && playbackPosition <= 1.0f)
        {
            float playheadX = playbackPosition * bounds.getWidth();
            
            // Vertical line
            g.setColour(juce::Colour(0xffffb347).withAlpha(0.9f));
            g.drawLine(playheadX, 0, playheadX, stepHeight, 2.0f);
            
            // Small triangle at top
            juce::Path triangle;
            triangle.addTriangle(playheadX - 5, 0, playheadX + 5, 0, playheadX, 8);
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(triangle);
        }
    }
    
    void setStepPattern(const std::array<bool, 16>& pattern)
    {
        stepPattern = pattern;
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
        int stepIndex = getStepIndexFromX(event.x);
        if (stepIndex >= 0 && stepIndex < 16)
        {
            if (onStepClicked)
                onStepClicked(stepIndex);
        }
    }
    
    std::function<void(int)> onStepClicked;
    
private:
    std::array<bool, 16> stepPattern = {};
    int currentStep = 0;
    bool isPlaying = false;
    float playbackPosition = -1.0f;  // -1 = hidden, 0.0-1.0 = normalized position
    juce::Colour stripColor = juce::Colour(0xff6f93c8);  // Default muted blue
    
    int getStepIndexFromX(int x)
    {
        float stepWidth = getWidth() / 16.0f;
        return juce::jlimit(0, 15, static_cast<int>(x / stepWidth));
    }
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepSequencerDisplay)
};
