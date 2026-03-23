#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace PluginEditorStyle
{

inline const auto kBgTop = juce::Colour(0xff232629);
inline const auto kBgBottom = juce::Colour(0xff16181a);
inline const auto kPanelTop = juce::Colour(0xff36393d);
inline const auto kPanelBottom = juce::Colour(0xff272a2d);
inline const auto kPanelStroke = juce::Colour(0xff70757a);
inline const auto kPanelInnerStroke = juce::Colour(0xff242424);
inline const auto kAccent = juce::Colour(0xffffb347);
inline const auto kTextPrimary = juce::Colour(0xffefefef);
inline const auto kTextSecondary = juce::Colour(0xffc3c3c3);
inline const auto kTextMuted = juce::Colour(0xff969696);
inline const auto kSurfaceDark = juce::Colour(0xff1a1a1a);

inline void drawPanel(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour accent, float radius = 8.0f)
{
    g.setColour(juce::Colours::black.withAlpha(0.2f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 1.5f), radius);

    juce::ColourGradient fill(kPanelTop, bounds.getX(), bounds.getY(),
                              kPanelBottom, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(bounds, radius);

    juce::ColourGradient topSheen(juce::Colours::white.withAlpha(0.06f), bounds.getX(), bounds.getY(),
                                  juce::Colours::transparentWhite, bounds.getX(), bounds.getY() + (bounds.getHeight() * 0.33f), false);
    g.setGradientFill(topSheen);
    g.fillRoundedRectangle(bounds.reduced(1.0f), juce::jmax(2.0f, radius - 1.0f));

    g.setColour(kPanelStroke);
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);

    g.setColour(accent.withAlpha(0.22f));
    g.drawRoundedRectangle(bounds.reduced(1.5f), juce::jmax(2.0f, radius - 1.5f), 1.0f);

    g.setColour(kPanelInnerStroke);
    g.drawRoundedRectangle(bounds.reduced(2.0f), juce::jmax(2.0f, radius - 2.0f), 1.0f);
}

inline void styleUiButton(juce::Button& button, bool primary = false)
{
    button.setColour(juce::TextButton::buttonColourId,
                     primary ? kAccent.withAlpha(0.9f) : juce::Colour(0xff3b4146));
    button.setColour(juce::TextButton::buttonOnColourId,
                     primary ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
    button.setColour(juce::TextButton::textColourOffId,
                     primary ? juce::Colour(0xff141414) : kTextPrimary);
    button.setColour(juce::TextButton::textColourOnId,
                     primary ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
}

inline void styleUiCombo(juce::ComboBox& combo)
{
    combo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff353b42));
    combo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff6a7076));
    combo.setColour(juce::ComboBox::textColourId, kTextPrimary);
    combo.setColour(juce::ComboBox::arrowColourId, kAccent.brighter(0.08f));
    combo.setJustificationType(juce::Justification::centredLeft);
}

inline void enableAltClickReset(juce::Slider& slider, double defaultValue)
{
    slider.setDoubleClickReturnValue(true, defaultValue);
}

} // namespace PluginEditorStyle
