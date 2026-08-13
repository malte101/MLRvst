#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Single source of truth for the editor's visual system.
// Depth is expressed by flat surface value steps (no gradients, sheens, or
// stacked strokes); amber is reserved for selected/focused/primary-action.
namespace PluginEditorStyle
{

// ---- Elevation ramp (dark → light = recessed → raised) ----
inline const auto kWell = juce::Colour(0xff121417);          // waveforms, meters, recessed canvases
inline const auto kBg = juce::Colour(0xff141619);            // page background
inline const auto kInset = juce::Colour(0xff1b1e21);         // recessed content: tracks, wells, pill rails
inline const auto kSurface = juce::Colour(0xff23262a);       // panels / cards
inline const auto kSurfaceHi = juce::Colour(0xff2b2f34);     // controls at rest
inline const auto kSurfaceHover = juce::Colour(0xff343940);  // hover fill
inline const auto kStroke = juce::Colour(0xff33383e);        // the one hairline

// ---- Identity + semantic colors ----
inline const auto kAccent = juce::Colour(0xffffb347);        // selected / focused / primary action ONLY
inline const auto kAccentInk = juce::Colour(0xff141414);     // text on solid accent
inline const auto kLive = juce::Colour(0xff77c982);          // playing / active / connected
inline const auto kRec = juce::Colour(0xffd46b62);           // recording / armed / destructive
inline const auto kDataNeutral = juce::Colour(0xff8a9096);   // unassigned / fallback data color
inline const auto kModAmber = juce::Colour(0xffffd24a);      // modulation-active glow
inline const auto kModCyan = juce::Colour(0xff3bd5ff);       // modulation glow fallback near-yellow bases

// ---- Text ramp ----
inline const auto kTextPrimary = juce::Colour(0xffe8ecec);
inline const auto kTextSecondary = juce::Colour(0xffb6bcc2);
inline const auto kTextMuted = juce::Colour(0xff8a9096);

// ---- Legacy aliases (values remapped into the flat system; sweep out over time) ----
inline const auto kBgTop = kBg;
inline const auto kBgBottom = kBg;
inline const auto kPanelTop = kSurface;
inline const auto kPanelBottom = kSurface;
inline const auto kPanelStroke = kStroke;
inline const auto kPanelInnerStroke = juce::Colours::transparentBlack;
inline const auto kSurfaceDark = kWell;

// ---- Shape ----
constexpr float kRadiusS = 4.0f;   // buttons, chips, combos, thumbs
constexpr float kRadiusL = 8.0f;   // panels, cards, scene slots

// ---- Spacing (4px grid) + hit-target floor ----
constexpr int kUnit = 4;
constexpr int kGapS = 4;
constexpr int kGapM = 8;
constexpr int kGapL = 12;
constexpr int kPadPanel = 8;
constexpr int kHitMin = 24;        // min height for always-visible interactive controls

// ---- Type roles (9pt stage floor; weight carries hierarchy, not size) ----
enum class FontRole { Micro, Label, Body, Title, Display };

inline juce::Font roleFont(FontRole role, bool emphasized = false)
{
    const auto weight = emphasized ? juce::Font::bold : juce::Font::plain;
    switch (role)
    {
        case FontRole::Micro:   return juce::Font(juce::FontOptions(9.0f, weight));
        case FontRole::Label:   return juce::Font(juce::FontOptions(10.0f, weight));
        case FontRole::Body:    return juce::Font(juce::FontOptions(11.5f, weight));
        case FontRole::Title:   return juce::Font(juce::FontOptions(13.0f, juce::Font::bold));
        case FontRole::Display: return juce::Font(juce::FontOptions(17.0f, juce::Font::bold));
    }
    return juce::Font(juce::FontOptions(11.5f, weight));
}

// Flat card: one fill, one hairline, and the accent as a 3px identity edge on
// the left (clipped to the rounded corner) instead of a full accent ring.
inline void drawPanel(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour accent, float radius = kRadiusL)
{
    g.setColour(kSurface);
    g.fillRoundedRectangle(bounds, radius);

    g.setColour(kStroke);
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);

    juce::Graphics::ScopedSaveState save(g);
    juce::Path clip;
    clip.addRoundedRectangle(bounds, radius);
    g.reduceClipRegion(clip);
    g.setColour(accent);
    g.fillRect(juce::Rectangle<float>(bounds.getX(), bounds.getY(), 3.0f, bounds.getHeight()));
}

inline void styleUiButton(juce::Button& button, bool primary = false)
{
    button.setColour(juce::TextButton::buttonColourId, primary ? kAccent : kSurfaceHi);
    button.setColour(juce::TextButton::buttonOnColourId, primary ? kAccent.brighter(0.08f) : kAccent.withAlpha(0.9f));
    button.setColour(juce::TextButton::textColourOffId, primary ? kAccentInk : kTextPrimary);
    button.setColour(juce::TextButton::textColourOnId, kAccentInk);
}

inline void styleUiCombo(juce::ComboBox& combo)
{
    combo.setColour(juce::ComboBox::backgroundColourId, kSurfaceHi);
    combo.setColour(juce::ComboBox::outlineColourId, kStroke);
    combo.setColour(juce::ComboBox::textColourId, kTextPrimary);
    combo.setColour(juce::ComboBox::arrowColourId, kTextMuted);
    combo.setJustificationType(juce::Justification::centredLeft);
}

inline void enableAltClickReset(juce::Slider& slider, double defaultValue)
{
    slider.setDoubleClickReturnValue(true, defaultValue);
}

// One knob feel everywhere: straight up/down drag (JUCE's plain Rotary style
// tracks the mouse ANGLE around the knob, and HorizontalVerticalDrag cancels
// itself out on diagonal drags — both feel broken), no velocity acceleration,
// and a predictable number of pixels for the full range.
inline void styleRotaryDrag(juce::Slider& slider, int dragSensitivityPixels = 200)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setVelocityBasedMode(false);
    // Without this, holding ctrl/alt/cmd during a drag silently flips JUCE into
    // velocity mode (hidden cursor, warped response).
    slider.setVelocityModeParameters(1.0, 1, 0.0, false);
    slider.setMouseDragSensitivity(dragSensitivityPixels);
}

// TextButton that ignores popup-menu (right/ctrl) clicks itself, so onClick
// never fires for them. Parent components still receive the events through
// addMouseListener and can run their own right-click actions (delete,
// context menus) without the button activating first.
struct PopupSafeButton : juce::TextButton
{
    using juce::TextButton::TextButton;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::TextButton::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::TextButton::mouseUp(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::TextButton::mouseDrag(e);
    }
};

} // namespace PluginEditorStyle
