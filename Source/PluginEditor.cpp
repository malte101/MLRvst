/*
  ==============================================================================

    PluginEditor.cpp
    Modern Comprehensive UI Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PlayheadSpeedQuantizer.h"
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
const auto kBgTop = juce::Colour(0xff232629);
const auto kBgBottom = juce::Colour(0xff16181a);
const auto kPanelTop = juce::Colour(0xff36393d);
const auto kPanelBottom = juce::Colour(0xff272a2d);
const auto kPanelStroke = juce::Colour(0xff70757a);
const auto kPanelInnerStroke = juce::Colour(0xff242424);
const auto kAccent = juce::Colour(0xffffb347);
const auto kTextPrimary = juce::Colour(0xffefefef);
const auto kTextSecondary = juce::Colour(0xffc3c3c3);
const auto kTextMuted = juce::Colour(0xff969696);
const auto kSurfaceDark = juce::Colour(0xff1a1a1a);

void drawPanel(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour accent, float radius = 8.0f)
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

void enableAltClickReset(juce::Slider& slider, double defaultValue)
{
    // JUCE supports modifier-click reset when a double-click return value is set.
    slider.setDoubleClickReturnValue(true, defaultValue);
}

void styleUiButton(juce::Button& button, bool primary = false)
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

void styleUiCombo(juce::ComboBox& combo)
{
    combo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff32363a));
    combo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff5a5f64));
    combo.setColour(juce::ComboBox::textColourId, kTextPrimary);
    combo.setColour(juce::ComboBox::arrowColourId, kTextSecondary);
}

juce::String getGrainArpModeName(int mode)
{
    switch (juce::jlimit(0, 5, mode))
    {
        case 0: return "Octave";
        case 1: return "Power";
        case 2: return "Zigzag";
        case 3: return "Major";
        case 4: return "Minor";
        case 5: return "Penta";
        default: break;
    }
    return "Octave";
}

juce::String getPlayheadSpeedLabel(float ratio)
{
    return juce::String(PlayheadSpeedQuantizer::labelForRatio(ratio));
}

float getPlayheadSpeedRatioForStrip(const EnhancedAudioStrip& strip)
{
    return PlayheadSpeedQuantizer::quantizeRatio(strip.getPlayheadSpeedRatio());
}

juce::String getMonomePageDisplayName(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Normal: return "Normal";
        case MlrVSTAudioProcessor::ControlMode::Speed: return "Speed";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "Pitch";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "Pan";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "Volume";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "Grain Size";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "Filter";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "Swing";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "Gate";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "Modulation";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "Preset Loader";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "Step Edit";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "Group Assign";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "File Browser";
    }
    return "Normal";
}

juce::String getMonomePageShortName(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Speed: return "SPD";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "PIT";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "PAN";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "VOL";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "GRN";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "FLT";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "SWG";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "GATE";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "BRW";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "GRP";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "MOD";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "PST";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "STEP";
        case MlrVSTAudioProcessor::ControlMode::Normal:
        default: return "NORM";
    }
}

int modTargetToComboId(ModernAudioEngine::ModTarget target)
{
    switch (target)
    {
        case ModernAudioEngine::ModTarget::Volume: return 2;
        case ModernAudioEngine::ModTarget::Pan: return 3;
        case ModernAudioEngine::ModTarget::Pitch: return 4;
        case ModernAudioEngine::ModTarget::Speed: return 5;
        case ModernAudioEngine::ModTarget::Cutoff: return 6;
        case ModernAudioEngine::ModTarget::Resonance: return 7;
        case ModernAudioEngine::ModTarget::GrainSize: return 8;
        case ModernAudioEngine::ModTarget::GrainDensity: return 9;
        case ModernAudioEngine::ModTarget::GrainPitch: return 10;
        case ModernAudioEngine::ModTarget::GrainPitchJitter: return 11;
        case ModernAudioEngine::ModTarget::GrainSpread: return 12;
        case ModernAudioEngine::ModTarget::GrainJitter: return 13;
        case ModernAudioEngine::ModTarget::GrainRandom: return 14;
        case ModernAudioEngine::ModTarget::GrainArp: return 15;
        case ModernAudioEngine::ModTarget::GrainCloud: return 16;
        case ModernAudioEngine::ModTarget::GrainEmitter: return 17;
        case ModernAudioEngine::ModTarget::GrainEnvelope: return 18;
        case ModernAudioEngine::ModTarget::Retrigger: return 19;
        case ModernAudioEngine::ModTarget::None:
        default: return 1;
    }
}

ModernAudioEngine::ModTarget comboIdToModTarget(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::ModTarget::Volume;
        case 3: return ModernAudioEngine::ModTarget::Pan;
        case 4: return ModernAudioEngine::ModTarget::Pitch;
        case 5: return ModernAudioEngine::ModTarget::Speed;
        case 6: return ModernAudioEngine::ModTarget::Cutoff;
        case 7: return ModernAudioEngine::ModTarget::Resonance;
        case 8: return ModernAudioEngine::ModTarget::GrainSize;
        case 9: return ModernAudioEngine::ModTarget::GrainDensity;
        case 10: return ModernAudioEngine::ModTarget::GrainPitch;
        case 11: return ModernAudioEngine::ModTarget::GrainPitchJitter;
        case 12: return ModernAudioEngine::ModTarget::GrainSpread;
        case 13: return ModernAudioEngine::ModTarget::GrainJitter;
        case 14: return ModernAudioEngine::ModTarget::GrainRandom;
        case 15: return ModernAudioEngine::ModTarget::GrainArp;
        case 16: return ModernAudioEngine::ModTarget::GrainCloud;
        case 17: return ModernAudioEngine::ModTarget::GrainEmitter;
        case 18: return ModernAudioEngine::ModTarget::GrainEnvelope;
        case 19: return ModernAudioEngine::ModTarget::Retrigger;
        case 1:
        default: return ModernAudioEngine::ModTarget::None;
    }
}

bool modTargetAllowsBipolar(ModernAudioEngine::ModTarget target)
{
    return ModernAudioEngine::modTargetSupportsBipolar(target);
}

enum class StepCellModifierGesture
{
    None = 0,
    Divide,
    RampUp,
    RampDown
};

StepCellModifierGesture getStepCellModifierGesture(const juce::ModifierKeys& mods)
{
    // Keep modifier priority aligned with StepSequencerDisplay cell editing.
    if (mods.isCommandDown())
        return StepCellModifierGesture::Divide;
    if (mods.isCtrlDown())
        return StepCellModifierGesture::RampUp;
    if (mods.isAltDown())
        return StepCellModifierGesture::RampDown;
    return StepCellModifierGesture::None;
}

float shapeCurvePhaseUi(float phase01, float bend, ModernAudioEngine::ModCurveShape shape);

float sampleModSubdivisionValueUi(float startValue,
                                  float endValue,
                                  int subdivisions,
                                  float phase01)
{
    const float start = juce::jlimit(0.0f, 1.0f, startValue);
    const float end = juce::jlimit(0.0f, 1.0f, endValue);
    const int subdiv = juce::jlimit(1, ModernAudioEngine::ModMaxStepSubdivisions, subdivisions);
    const float phase = juce::jlimit(0.0f, 0.999999f, phase01);

    if (subdiv <= 1)
        return start;

    const float subdivPos = phase * static_cast<float>(subdiv);
    const int subdivIndex = juce::jlimit(0, subdiv - 1, static_cast<int>(std::floor(subdivPos)));
    const float t = static_cast<float>(subdivIndex) / static_cast<float>(juce::jmax(1, subdiv - 1));
    return juce::jlimit(0.0f, 1.0f, start + ((end - start) * t));
}

void computeSingleModCellRamp(float sourceStart,
                              float sourceEnd,
                              int deltaY,
                              bool rampUpMode,
                              float& outStart,
                              float& outEnd)
{
    const float clampedStart = juce::jlimit(0.0f, 1.0f, sourceStart);
    const float clampedEnd = juce::jlimit(0.0f, 1.0f, sourceEnd);
    float peak = juce::jlimit(0.0f, 1.0f, juce::jmax(clampedStart, clampedEnd));
    if (peak < 0.001f)
        peak = 1.0f;

    const float depth = juce::jlimit(0.0f, 1.0f, 0.5f + (static_cast<float>(-deltaY) / 160.0f));
    const float low = juce::jlimit(0.0f, 1.0f, peak * (1.0f - depth));

    if (rampUpMode)
    {
        outStart = low;
        outEnd = peak;
    }
    else
    {
        outStart = peak;
        outEnd = low;
    }
}

int pitchScaleToComboId(ModernAudioEngine::PitchScale scale)
{
    switch (scale)
    {
        case ModernAudioEngine::PitchScale::Chromatic: return 1;
        case ModernAudioEngine::PitchScale::Major: return 2;
        case ModernAudioEngine::PitchScale::Minor: return 3;
        case ModernAudioEngine::PitchScale::Dorian: return 4;
        case ModernAudioEngine::PitchScale::PentatonicMinor: return 5;
        default: return 1;
    }
}

ModernAudioEngine::PitchScale comboIdToPitchScale(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::PitchScale::Major;
        case 3: return ModernAudioEngine::PitchScale::Minor;
        case 4: return ModernAudioEngine::PitchScale::Dorian;
        case 5: return ModernAudioEngine::PitchScale::PentatonicMinor;
        case 1:
        default: return ModernAudioEngine::PitchScale::Chromatic;
    }
}

int curveShapeToComboId(ModernAudioEngine::ModCurveShape shape)
{
    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::Linear: return 1;
        case ModernAudioEngine::ModCurveShape::ExponentialUp: return 2;
        case ModernAudioEngine::ModCurveShape::ExponentialDown: return 3;
        case ModernAudioEngine::ModCurveShape::Sine: return 4;
        case ModernAudioEngine::ModCurveShape::Square: return 5;
        default: return 1;
    }
}

ModernAudioEngine::ModCurveShape comboIdToCurveShape(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::ModCurveShape::ExponentialUp;
        case 3: return ModernAudioEngine::ModCurveShape::ExponentialDown;
        case 4: return ModernAudioEngine::ModCurveShape::Sine;
        case 5: return ModernAudioEngine::ModCurveShape::Square;
        case 1:
        default: return ModernAudioEngine::ModCurveShape::Linear;
    }
}

float shapeCurvePhaseUi(float phase01, float bend, ModernAudioEngine::ModCurveShape shape)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);

    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::Linear:
            return t;
        case ModernAudioEngine::ModCurveShape::ExponentialUp:
        {
            const float exp = 1.0f + (15.0f * amount);
            return std::pow(t, exp);
        }
        case ModernAudioEngine::ModCurveShape::ExponentialDown:
        {
            const float exp = 1.0f + (15.0f * amount);
            return 1.0f - std::pow(1.0f - t, exp);
        }
        case ModernAudioEngine::ModCurveShape::Sine:
        {
            const float phase = juce::jlimit(0.0f, 1.0f, t + (b * 0.45f));
            return 0.5f - (0.5f * std::cos(phase * juce::MathConstants<float>::pi));
        }
        case ModernAudioEngine::ModCurveShape::Square:
        {
            const float duty = juce::jlimit(0.02f, 0.98f, 0.5f + (b * 0.45f));
            return (t >= duty) ? 1.0f : 0.0f;
        }
        default:
            return t;
    }
}

float shapeSubdivisionBendPhaseUi(float phase01, float bend)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);
    const float exp = 1.0f + (18.0f * amount);
    return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
}

}

//==============================================================================
// WaveformDisplay Implementation
//==============================================================================

WaveformDisplay::WaveformDisplay()
{
    setOpaque(false);
}

void WaveformDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Safety check for invalid bounds
    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0 || 
        !std::isfinite(bounds.getWidth()) || !std::isfinite(bounds.getHeight()))
        return;
    
    // Background with depth so grain overlays read clearly.
    juce::ColourGradient bgGrad(kSurfaceDark.brighter(0.12f), bounds.getX(), bounds.getY(),
                                kSurfaceDark.darker(0.22f), bounds.getRight(), bounds.getBottom(), false);
    g.setGradientFill(bgGrad);
    g.fillRoundedRectangle(bounds, 4.0f);

    g.setColour(kPanelStroke.withAlpha(0.85f));
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
    
    if (!hasAudio)
    {
        // Keep the gradient look, but tint it with the strip color so empty strips
        // feel connected to their lane identity.
        const auto tint = waveformColor.withAlpha(0.18f);
        juce::ColourGradient emptyGrad(
            kSurfaceDark.brighter(0.16f).interpolatedWith(tint.brighter(0.45f), 0.26f),
            bounds.getX(), bounds.getY(),
            kSurfaceDark.darker(0.24f).interpolatedWith(tint.darker(0.35f), 0.22f),
            bounds.getRight(), bounds.getBottom(),
            false);
        g.setGradientFill(emptyGrad);
        g.fillRoundedRectangle(bounds.reduced(0.5f), 4.0f);

        // "No Sample" text (like reference image)
        g.setColour(kTextMuted);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText("No Sample", bounds, juce::Justification::centred);
        return;
    }

    const juce::Colour grainAccent = waveformColor.interpolatedWith(kAccent, 0.35f)
                                       .withMultipliedSaturation(1.1f)
                                       .withMultipliedBrightness(1.08f);
    
    // Draw waveform
    if (!thumbnail.empty())
    {
        juce::Path waveformPath;
        auto width = bounds.getWidth();
        auto height = bounds.getHeight();
        auto centerY = height * 0.5f;
        
        waveformPath.startNewSubPath(0, centerY);
        
        for (size_t i = 0; i < thumbnail.size(); ++i)
        {
            auto x = (i / static_cast<float>(thumbnail.size())) * width;
            auto y = centerY - (thumbnail[i] * centerY * 0.9f);
            
            // Safety check for valid coordinates
            if (std::isfinite(x) && std::isfinite(y))
                waveformPath.lineTo(x, y);
        }
        
        // Mirror bottom half
        for (int i = static_cast<int>(thumbnail.size()) - 1; i >= 0; --i)
        {
            auto x = (i / static_cast<float>(thumbnail.size())) * width;
            auto y = centerY + (thumbnail[static_cast<size_t>(i)] * centerY * 0.9f);
            
            // Safety check for valid coordinates
            if (std::isfinite(x) && std::isfinite(y))
                waveformPath.lineTo(x, y);
        }
        
        waveformPath.closeSubPath();
        
        // Fill waveform with custom color
        g.setColour(waveformColor.withAlpha(0.5f));
        g.fillPath(waveformPath);
        
        // Outline
        g.setColour(waveformColor.brighter(0.2f));
        g.strokePath(waveformPath, juce::PathStrokeType(1.35f));
    }
    
    // Draw loop points with matching waveform color
    if (maxColumns > 0)
    {
        auto loopStartX = (loopStart / static_cast<float>(maxColumns)) * bounds.getWidth();
        auto loopEndX = (loopEnd / static_cast<float>(maxColumns)) * bounds.getWidth();
        auto rectWidth = loopEndX - loopStartX;
        auto rectHeight = bounds.getHeight();
        
        // Strict safety check - JUCE requires positive, finite dimensions
        if (std::isfinite(loopStartX) && std::isfinite(loopEndX) && 
            std::isfinite(rectWidth) && std::isfinite(rectHeight) &&
            rectWidth > 0.0f && rectHeight > 0.0f &&
            loopStartX >= 0.0f && loopStartX < bounds.getWidth())
        {
            // Fill with transparent waveform color
            g.setColour(waveformColor.withAlpha(0.25f));
            g.fillRect(loopStartX, 0.0f, rectWidth, rectHeight);
            
            // Draw loop markers with semi-transparent waveform color
            g.setColour(waveformColor.withAlpha(0.95f));
            g.drawLine(loopStartX, 0.0f, loopStartX, rectHeight, 2.0f);
            g.drawLine(loopEndX, 0.0f, loopEndX, rectHeight, 2.0f);
        }
    }
    
    // Draw playback position with matching waveform color (darker)
    if (std::isfinite(playbackPosition) && playbackPosition >= 0.0 && playbackPosition <= 1.0)
    {
        auto playX = playbackPosition * bounds.getWidth();
        if (std::isfinite(playX))
        {
            if (grainWindowOverlayEnabled && grainWindowNorm > 0.0)
            {
                const auto winW = juce::jlimit(1.0f,
                                               bounds.getWidth(),
                                               static_cast<float>(grainWindowNorm * bounds.getWidth()));
                auto x0 = static_cast<float>(playX) - (winW * 0.5f);
                x0 = juce::jlimit(0.0f, bounds.getWidth() - winW, x0);
                auto windowRect = juce::Rectangle<float>(x0, 0.0f, winW, bounds.getHeight()).reduced(0.0f, 1.0f);
                juce::ColourGradient winGrad(grainAccent.withAlpha(0.08f), windowRect.getX(), windowRect.getY(),
                                             grainAccent.withAlpha(0.24f), windowRect.getCentreX(), windowRect.getCentreY(), true);
                g.setGradientFill(winGrad);
                g.fillRoundedRectangle(windowRect, 2.5f);
                g.setColour(grainAccent.withAlpha(0.42f));
                g.drawRoundedRectangle(windowRect, 2.5f, 1.0f);
            }

            g.setColour(grainAccent.withAlpha(0.2f));
            g.drawLine(static_cast<float>(playX), 0.0f, static_cast<float>(playX),
                       static_cast<float>(bounds.getHeight()), 7.0f);
            g.setColour(grainAccent.withAlpha(0.98f));
            g.drawLine(static_cast<float>(playX), 0.0f, static_cast<float>(playX),
                       static_cast<float>(bounds.getHeight()), 2.0f);
            g.fillEllipse(static_cast<float>(playX) - 2.6f, 1.0f, 5.2f, 5.2f);
        }
    }

    // Draw slice markers overlay for active mode only.
    if (waveformTotalSamples > 0)
    {
        const auto drawSliceSet = [&](const std::array<int, 16>& slices, juce::Colour colour, float thickness)
        {
            g.setColour(colour);
            for (int i = 0; i < 16; ++i)
            {
                const float norm = juce::jlimit(0.0f, 1.0f,
                                                static_cast<float>(slices[static_cast<size_t>(i)])
                                                / static_cast<float>(juce::jmax(1, waveformTotalSamples - 1)));
                const float x = norm * bounds.getWidth();
                if (std::isfinite(x))
                    g.drawLine(x, 0.0f, x, bounds.getHeight(), thickness);
            }
        };

        const auto markerColor = waveformColor.withAlpha(transientSlicesActive ? 0.95f : 0.7f);
        if (transientSlicesActive)
            drawSliceSet(transientSliceSamples, markerColor, 1.7f);
        else
            drawSliceSet(normalSliceSamples, markerColor, 1.2f);
    }

    // Draw column dividers
    g.setColour(juce::Colour(0xff4a4a4a).withAlpha(grainWindowOverlayEnabled ? 0.55f : 1.0f));
    for (int i = 1; i < maxColumns; ++i)
    {
        auto x = (i / static_cast<float>(maxColumns)) * bounds.getWidth();
        if (std::isfinite(x))
            g.drawLine(x, 0, x, bounds.getHeight(), 0.5f);
    }

    if (grainWindowOverlayEnabled)
    {
        g.setColour(grainAccent.withAlpha(0.22f));
        int markerIdx = 0;
        const float markerHalfHeight = 6.0f;
        const float markerRadius = 3.2f;
        const float markerGlowRadius = 6.4f;
        const float edgePad = juce::jmax(markerHalfHeight, markerGlowRadius) + 1.0f;
        const float maxPitchTravel = juce::jmax(1.0f, (bounds.getHeight() * 0.5f) - edgePad);
        for (const float marker : grainMarkerPositions)
        {
            if (marker < 0.0f || marker > 1.0f || !std::isfinite(marker))
            {
                ++markerIdx;
                continue;
            }
            const float x = marker * bounds.getWidth();
            float pitchNorm = juce::jlimit(-1.0f, 1.0f, grainHudPitchSemitones / 48.0f);
            if (markerIdx >= 0 && markerIdx < static_cast<int>(grainMarkerPitchNorms.size()))
            {
                const float markerPitchNorm = grainMarkerPitchNorms[static_cast<size_t>(markerIdx)];
                if (std::isfinite(markerPitchNorm))
                    pitchNorm = juce::jlimit(-1.0f, 1.0f, markerPitchNorm);
            }
            const float jitterNorm = juce::jlimit(0.0f, 1.0f, grainHudPitchJitterSemitones / 48.0f);
            const float phase = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.0025);
            const float yBase = (bounds.getHeight() * 0.5f) - (pitchNorm * maxPitchTravel);
            const float yJitter = std::sin((static_cast<float>(markerIdx) * 1.3f) + phase)
                * (grainHudArpDepth * 0.08f + jitterNorm * 0.12f) * bounds.getHeight();
            const float yCenter = juce::jlimit(edgePad, bounds.getHeight() - edgePad, yBase + yJitter);
            g.drawLine(x, yCenter - markerHalfHeight, x, yCenter + markerHalfHeight, 2.4f);
            g.setColour(grainAccent.withAlpha(0.84f));
            g.fillEllipse(x - markerRadius, yCenter - markerRadius, markerRadius * 2.0f, markerRadius * 2.0f);
            g.setColour(grainAccent.withAlpha(0.26f));
            g.fillEllipse(x - markerGlowRadius, yCenter - markerGlowRadius, markerGlowRadius * 2.0f, markerGlowRadius * 2.0f);
            g.setColour(grainAccent.withAlpha(0.22f));
            ++markerIdx;
        }
    }

    if (grainHudOverlayEnabled)
    {
        auto hud = bounds.reduced(6.0f);
        auto hudW = juce::jlimit(150.0f, bounds.getWidth() - 8.0f, bounds.getWidth() * 0.56f);
        auto hudH = juce::jlimit(22.0f, bounds.getHeight() - 8.0f, bounds.getHeight() * 0.45f);
        auto hudRect = juce::Rectangle<float>(hud.getRight() - hudW, hud.getY() + 2.0f, hudW, hudH);
        g.setColour(juce::Colour(0xff121212).withAlpha(0.72f));
        g.fillRoundedRectangle(hudRect, 3.0f);
        g.setColour(grainAccent.withAlpha(0.4f));
        g.drawRoundedRectangle(hudRect, 3.0f, 0.9f);

        auto textRect = hudRect.reduced(5.0f, 2.5f);
        g.setColour(kTextSecondary.withAlpha(0.95f));
        g.setFont(juce::Font(juce::FontOptions(8.4f, juce::Font::bold)));
        g.drawText(grainHudLineA, textRect.removeFromTop(8.8f), juce::Justification::left, false);
        g.setColour(kTextMuted.withAlpha(0.98f));
        g.setFont(juce::Font(juce::FontOptions(7.8f)));
        g.drawText(grainHudLineB, textRect.removeFromTop(8.5f), juce::Justification::left, false);

        auto bars = hudRect.removeFromBottom(5.0f).reduced(5.0f, 0.0f);
        auto drawHudBar = [&](float value, juce::Colour c)
        {
            const float clamped = juce::jlimit(0.0f, 1.0f, value);
            auto slot = bars.removeFromLeft((bars.getWidth() / 3.0f) - 1.0f);
            g.setColour(juce::Colours::black.withAlpha(0.3f));
            g.fillRoundedRectangle(slot, 1.4f);
            g.setColour(c.withAlpha(0.85f));
            g.fillRoundedRectangle(slot.withWidth(slot.getWidth() * clamped), 1.4f);
            bars.removeFromLeft(1.0f);
        };
        drawHudBar(grainHudDensity, waveformColor.withMultipliedBrightness(1.1f));
        drawHudBar(grainHudSpread, grainAccent.withMultipliedBrightness(1.05f));
        drawHudBar(grainHudEmitter, grainAccent.brighter(0.22f));
    }
}

void WaveformDisplay::resized()
{
}

void WaveformDisplay::setAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    (void) sampleRate;
    hasAudio = buffer.getNumSamples() > 0;
    if (!hasAudio)
    {
        clear();
        return;
    }

    generateThumbnail(buffer);
    repaint();
}

void WaveformDisplay::generateThumbnail(const juce::AudioBuffer<float>& buffer)
{
    const int thumbnailSize = 512;
    thumbnail.clear();
    thumbnail.resize(static_cast<size_t>(thumbnailSize), 0.0f);
    
    auto numSamples = buffer.getNumSamples();
    if (numSamples == 0) return;
    
    auto samplesPerPixel = numSamples / thumbnailSize;
    
    for (int i = 0; i < thumbnailSize; ++i)
    {
        float maxVal = 0.0f;
        auto startSample = i * samplesPerPixel;
        auto endSample = juce::jmin((i + 1) * samplesPerPixel, numSamples);
        
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* channelData = buffer.getReadPointer(ch);
            for (int s = startSample; s < endSample; ++s)
            {
                maxVal = juce::jmax(maxVal, std::abs(channelData[s]));
            }
        }
        
        thumbnail[static_cast<size_t>(i)] = maxVal;
    }
}

void WaveformDisplay::setPlaybackPosition(double normalizedPosition)
{
    // Validate input to prevent NaN/Inf
    if (std::isfinite(normalizedPosition))
        playbackPosition = juce::jlimit(0.0, 1.0, normalizedPosition);
    else
        playbackPosition = 0.0;
    
    repaint();
}

void WaveformDisplay::setGrainWindowOverlay(bool enabled, double windowNorm)
{
    grainWindowOverlayEnabled = enabled;
    grainWindowNorm = juce::jlimit(0.0, 1.0, std::isfinite(windowNorm) ? windowNorm : 0.0);
    repaint();
}

void WaveformDisplay::setGrainMarkerPositions(const std::array<float, 8>& positions,
                                              const std::array<float, 8>& pitchNorms)
{
    grainMarkerPositions = positions;
    grainMarkerPitchNorms = pitchNorms;
    repaint();
}

void WaveformDisplay::setGrainHudOverlay(bool enabled,
                                         const juce::String& lineA,
                                         const juce::String& lineB,
                                         float density,
                                         float spread,
                                         float emitter,
                                         float pitchSemitones,
                                         float arpDepth,
                                         float pitchJitterSemitones)
{
    grainHudOverlayEnabled = enabled;
    grainHudLineA = lineA;
    grainHudLineB = lineB;
    grainHudDensity = juce::jlimit(0.0f, 1.0f, density);
    grainHudSpread = juce::jlimit(0.0f, 1.0f, spread);
    grainHudEmitter = juce::jlimit(0.0f, 1.0f, emitter);
    grainHudPitchSemitones = juce::jlimit(-48.0f, 48.0f, pitchSemitones);
    grainHudArpDepth = juce::jlimit(0.0f, 1.0f, arpDepth);
    grainHudPitchJitterSemitones = juce::jlimit(0.0f, 48.0f, pitchJitterSemitones);
    repaint();
}

void WaveformDisplay::setLoopPoints(int startCol, int endCol, int cols)
{
    loopStart = startCol;
    loopEnd = endCol;
    maxColumns = cols;
    repaint();
}

void WaveformDisplay::setSliceMarkers(const std::array<int, 16>& normalSlices,
                                      const std::array<int, 16>& transientSlices,
                                      int totalSamples,
                                      bool transientModeActive)
{
    normalSliceSamples = normalSlices;
    transientSliceSamples = transientSlices;
    waveformTotalSamples = juce::jmax(0, totalSamples);
    transientSlicesActive = transientModeActive;
    repaint();
}

void WaveformDisplay::clear()
{
    hasAudio = false;
    thumbnail.clear();
    playbackPosition = 0.0;
    waveformTotalSamples = 0;
    normalSliceSamples.fill(0);
    transientSliceSamples.fill(0);
    grainWindowOverlayEnabled = false;
    grainWindowNorm = 0.0;
    grainMarkerPositions.fill(-1.0f);
    grainMarkerPitchNorms.fill(0.0f);
    grainHudOverlayEnabled = false;
    grainHudLineA.clear();
    grainHudLineB.clear();
    grainHudDensity = 0.0f;
    grainHudSpread = 0.0f;
    grainHudEmitter = 0.0f;
    grainHudPitchSemitones = 0.0f;
    grainHudArpDepth = 0.0f;
    grainHudPitchJitterSemitones = 0.0f;
    repaint();
}

void WaveformDisplay::setWaveformColor(juce::Colour color)
{
    waveformColor = color;
    repaint();
}

//==============================================================================
// LevelMeter Implementation
//==============================================================================

LevelMeter::LevelMeter()
{
    setOpaque(false);
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Background
    g.setColour(kSurfaceDark);
    g.fillRoundedRectangle(bounds, 2.0f);
    
    // Border
    g.setColour(kPanelStroke);
    g.drawRoundedRectangle(bounds, 2.0f, 1.0f);
    
    // Level bar
    if (currentLevel > 0.0f)
    {
        float barHeight = bounds.getHeight() * currentLevel;
        auto barBounds = bounds.removeFromBottom(barHeight).reduced(2.0f);
        
        // Color based on level (green -> yellow -> red)
        juce::Colour barColor;
        if (currentLevel < 0.7f)
            barColor = juce::Colour(0xff6eb676);
        else if (currentLevel < 0.9f)
            barColor = juce::Colour(0xffd3b35c);
        else
            barColor = juce::Colour(0xffd46b62);
        
        g.setColour(barColor);
        g.fillRoundedRectangle(barBounds, 1.0f);
    }
    
    // Peak indicator (small line at peak level)
    if (peakLevel > 0.0f)
    {
        float peakY = bounds.getBottom() - (bounds.getHeight() * peakLevel);
        g.setColour(kTextPrimary);
        g.drawLine(bounds.getX() + 2, peakY, bounds.getRight() - 2, peakY, 1.0f);
    }
}

void LevelMeter::setLevel(float level)
{
    currentLevel = juce::jlimit(0.0f, 1.0f, level);
    
    // Update peak with decay
    if (currentLevel > peakLevel)
        peakLevel = currentLevel;
    else
        peakLevel *= 0.95f;  // Slow decay
    
    repaint();
}

void LevelMeter::setPeak(float peak)
{
    peakLevel = juce::jlimit(0.0f, 1.0f, peak);
    repaint();
}

//==============================================================================
// StripControl Implementation
//==============================================================================

//==============================================================================
// StripControl - Compact horizontal layout with LED overlay
//==============================================================================

StripControl::StripControl(int idx, MlrVSTAudioProcessor& p)
    : stripIndex(idx), processor(p), waveform()
{
    setupComponents();
    startTimer(30);
}

void StripControl::setupComponents()
{
    // Track palette uses muted tones closer to Ableton's default session colors.
    const juce::Colour trackColors[] = {
        juce::Colour(0xffd36f63),
        juce::Colour(0xffd18f4f),
        juce::Colour(0xffbda659),
        juce::Colour(0xff6faa6f),
        juce::Colour(0xff5ea5a8),
        juce::Colour(0xff6f93c8),
        juce::Colour(0xff9a82bc)
    };

    stripColor = trackColors[juce::jmax(0, stripIndex) % 7];
    
    // Setup colored knob look and feel
    knobLookAndFeel.setKnobColor(stripColor);
    
    // Strip label with colored text
    stripLabel.setText("S" + juce::String(stripIndex + 1), juce::dontSendNotification);
    stripLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    stripLabel.setJustificationType(juce::Justification::centredLeft);
    stripLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(stripLabel);
    
    // Waveform display with rainbow color
    waveform.setWaveformColor(stripColor.withMultipliedSaturation(1.35f).withMultipliedBrightness(1.25f));
    addAndMakeVisible(waveform);
    
    // Step sequencer display with matching rainbow color
    stepDisplay.setStripColor(stripColor);
    stepDisplay.onStepSet = [this](int stepIndex, bool enabled)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
            {
                const int totalSteps = strip->getStepTotalSteps();
                if (stepIndex >= 0 && stepIndex < totalSteps)
                    strip->setStepEnabledAtIndex(stepIndex, enabled, true);
            }
        }
    };
    stepDisplay.onStepSubdivisionSet = [this](int stepIndex, int subdivisions)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepSubdivisionAtIndex(stepIndex, subdivisions);
        }
    };
    stepDisplay.onStepVelocityRangeSet = [this](int stepIndex, float startVelocity, float endVelocity)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepSubdivisionVelocityRangeAtIndex(stepIndex, startVelocity, endVelocity);
        }
    };
    stepDisplay.onStepProbabilitySet = [this](int stepIndex, float probability)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepProbabilityAtIndex(stepIndex, probability);
        }
    };
    addChildComponent(stepDisplay);  // Hidden initially
    
    // Load button - compact
    loadButton.setButtonText("Load");
    loadButton.onClick = [this]() { loadSample(); };
    loadButton.setTooltip("Load sample into this strip.");
    addAndMakeVisible(loadButton);

    transientSliceButton.setButtonText("TIME");
    transientSliceButton.setClickingTogglesState(true);
    transientSliceButton.setTooltip("Toggle slice mapping: Time = 16 equal slices, Transient = onset-based slices");
    transientSliceButton.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setTransientSliceMode(transientSliceButton.getToggleState());
        }
    };
    addAndMakeVisible(transientSliceButton);

    // Play mode selector - compact (removed Reverse and Ping-Pong)
    playModeBox.addItem("One-Shot", 1);
    playModeBox.addItem("Loop", 2);
    playModeBox.addItem("Gate", 3);
    playModeBox.addItem("Step", 4);  // Step sequencer mode
    playModeBox.addItem("Grain", 5);
    playModeBox.setJustificationType(juce::Justification::centredLeft);
    playModeBox.setSelectedId(2);  // Default Loop
    playModeBox.setTooltip("Playback mode for this strip.");
    playModeBox.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            int modeId = playModeBox.getSelectedId() - 1;
            strip->setPlayMode(static_cast<EnhancedAudioStrip::PlayMode>(modeId));
            
            // Switch between waveform and step display
            bool isStepMode = (modeId == 3);  // Step mode is now index 3
            showingStepDisplay = isStepMode;
            
            waveform.setVisible(!isStepMode);
            stepDisplay.setVisible(isStepMode);
            patternLengthBox.setVisible(isStepMode);
            updateGrainOverlayVisibility();
            
            // Don't manually start - let process() auto-start when DAW plays
            // This respects the host transport state (paused or playing)
            
            resized();  // Re-layout components
            
            DBG("Strip " << stripIndex << " mode changed to " 
                << (isStepMode ? "STEP SEQUENCER" : "normal"));
        }
    };
    addAndMakeVisible(playModeBox);
    
    // Direction mode selector - NEW!
    directionModeBox.addItem("Normal", 1);
    directionModeBox.addItem("Reverse", 2);
    directionModeBox.addItem("Ping-Pong", 3);
    directionModeBox.addItem("Random", 4);
    directionModeBox.addItem("Rnd Walk", 5);
    directionModeBox.addItem("Rnd Slice", 6);
    directionModeBox.setJustificationType(juce::Justification::centredLeft);
    directionModeBox.setSelectedId(1);  // Default Normal
    directionModeBox.setTooltip("Playback direction behavior.");
    directionModeBox.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            int modeId = directionModeBox.getSelectedId() - 1;
            strip->setDirectionMode(static_cast<EnhancedAudioStrip::DirectionMode>(modeId));
            
            DBG("Strip " << stripIndex << " direction changed to " << modeId);
        }
    };
    addAndMakeVisible(directionModeBox);
    addAndMakeVisible(playModeBox);
    
    // Group selector - compact
    groupSelector.addItem("None", 1);
    groupSelector.addItem("G1", 2);
    groupSelector.addItem("G2", 3);
    groupSelector.addItem("G3", 4);
    groupSelector.addItem("G4", 5);
    groupSelector.setJustificationType(juce::Justification::centredLeft);
    groupSelector.setSelectedId(1);
    groupSelector.setTooltip("Assign strip to mute group.");
    groupSelector.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        
        // Get group ID: None=1, G1=2, G2=3, G3=4, G4=5
        // Convert to: None=-1, G1=0, G2=1, G3=2, G4=3
        int groupId = groupSelector.getSelectedId() - 2;
        
        // assignStripToGroup handles everything (removal from old, add to new, setGroup)
        processor.getAudioEngine()->assignStripToGroup(stripIndex, groupId);
    };
    addAndMakeVisible(groupSelector);
    
    // Compact rotary controls with colored look
    volumeSlider.setLookAndFeel(&knobLookAndFeel);
    volumeSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange(0.0, 1.0, 0.01);
    volumeSlider.setValue(1.0);
    enableAltClickReset(volumeSlider, 1.0);
    volumeSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(volumeSlider);
    
    volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripVolume" + juce::String(stripIndex), volumeSlider);
    
    panSlider.setLookAndFeel(&knobLookAndFeel);
    panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setRange(-1.0, 1.0, 0.01);
    panSlider.setValue(0.0);
    enableAltClickReset(panSlider, 0.0);
    panSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(panSlider);
    
    panAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripPan" + juce::String(stripIndex), panSlider);

    pitchSlider.setLookAndFeel(&knobLookAndFeel);
    pitchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pitchSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pitchSlider.setRange(-24.0, 24.0, 0.01);
    pitchSlider.setValue(0.0);
    enableAltClickReset(pitchSlider, 0.0);
    pitchSlider.setPopupDisplayEnabled(true, false, this);
    pitchSlider.setTextValueSuffix(" st");
    pitchSlider.setTooltip("Pitch offset in semitones.");
    addAndMakeVisible(pitchSlider);

    pitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripPitch" + juce::String(stripIndex), pitchSlider);

    speedSlider.setLookAndFeel(&knobLookAndFeel);
    speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    speedSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedSlider.setRange(0.0, 4.0, 0.001);
    speedSlider.setValue(1.0);
    enableAltClickReset(speedSlider, 1.0);
    speedSlider.textFromValueFunction = [](double value)
    {
        return getPlayheadSpeedLabel(PlayheadSpeedQuantizer::quantizeRatio(static_cast<float>(value)));
    };
    speedSlider.setPopupDisplayEnabled(true, false, this);
    speedSlider.setTooltip("Playhead speed (slice traversal only).");
    speedSlider.onValueChange = [this]()
    {
        const float quantizedRatio = PlayheadSpeedQuantizer::quantizeRatio(static_cast<float>(speedSlider.getValue()));
        if (std::abs(quantizedRatio - static_cast<float>(speedSlider.getValue())) > 1.0e-4f)
        {
            speedSlider.setValue(quantizedRatio, juce::sendNotificationSync);
            return;
        }

        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setPlayheadSpeedRatio(quantizedRatio);
    };
    addAndMakeVisible(speedSlider);
    
    speedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripSpeed" + juce::String(stripIndex), speedSlider);
    
    // Scratch amount
    scratchSlider.setLookAndFeel(&knobLookAndFeel);
    scratchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    scratchSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    scratchSlider.setRange(0.0, 100.0, 1.0);
    scratchSlider.setValue(0.0);
    enableAltClickReset(scratchSlider, 0.0);
    scratchSlider.textFromValueFunction = [this](double value)
    {
        const double clamped = juce::jlimit(0.0, 100.0, value);
        if (clamped <= 0.0001)
            return juce::String("0.00 s");

        double seconds = 0.0;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
            strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        {
            const double t = clamped / 100.0;
            seconds = juce::jlimit(0.015, 3.0, std::pow(t, 1.7) * 3.0);
        }
        else
        {
            double beats = 0.25;
            if (clamped <= 10.0)
            {
                const double t = clamped / 10.0;
                beats = 0.02 + (std::pow(t, 1.6) * 0.08);
            }
            else
            {
                const double t = (clamped - 10.0) / 90.0;
                beats = 0.10 + (std::pow(t, 1.8) * 7.90);
            }

            double tempo = 120.0;
            if (auto* engine = processor.getAudioEngine())
                tempo = juce::jmax(1.0, engine->getCurrentTempo());
            seconds = beats * (60.0 / tempo);
        }

        return juce::String(seconds, 2) + " s";
    };
    scratchSlider.setPopupDisplayEnabled(true, false, this);
    scratchSlider.setTooltip("Scratch amount. Higher values increase scratch gesture duration.");
    scratchSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setScratchAmount(static_cast<float>(scratchSlider.getValue()));
    };
    addAndMakeVisible(scratchSlider);

    sliceLengthSlider.setLookAndFeel(&knobLookAndFeel);
    sliceLengthSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    sliceLengthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sliceLengthSlider.setRange(0.0, 1.0, 0.001);
    sliceLengthSlider.setValue(1.0);
    enableAltClickReset(sliceLengthSlider, 1.0);
    sliceLengthSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "%";
    };
    sliceLengthSlider.setPopupDisplayEnabled(true, false, this);
    sliceLengthSlider.setTooltip("Loop segment length. 100% = full segment, lower values add click-free gaps.");
    addAndMakeVisible(sliceLengthSlider);
    sliceLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripSliceLength" + juce::String(stripIndex), sliceLengthSlider);

    auto setupGrainKnob = [this](juce::Slider& slider, juce::Label& label, const char* text,
                                 double min, double max, double step)
    {
        slider.setLookAndFeel(&knobLookAndFeel);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(min, max, step);
        enableAltClickReset(slider, juce::jlimit(min, max, 0.5 * (min + max)));
        slider.setPopupDisplayEnabled(true, false, this);
        addAndMakeVisible(slider);
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(9.2f, juce::Font::bold)));
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, stripColor.brighter(0.35f));
        addAndMakeVisible(label);
    };

    setupGrainKnob(grainSizeSlider, grainSizeLabel, "SIZE", 5.0, 2400.0, 1.0);
    setupGrainKnob(grainDensitySlider, grainDensityLabel, "DENS", 0.05, 0.9, 0.01);
    setupGrainKnob(grainPitchSlider, grainPitchLabel, "PITCH", -48.0, 48.0, 0.1);
    grainPitchSlider.getProperties().set("bipolarBase", true);
    grainPitchLabel.setJustificationType(juce::Justification::centredLeft);
    setupGrainKnob(grainPitchJitterSlider, grainPitchJitterLabel, "PJIT", 0.0, 48.0, 0.1);
    setupGrainKnob(grainSpreadSlider, grainSpreadLabel, "SPRD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainJitterSlider, grainJitterLabel, "SJTR", 0.0, 1.0, 0.01);
    setupGrainKnob(grainPositionJitterSlider, grainPositionJitterLabel, "POSJ", 0.0, 1.0, 0.01);
    setupGrainKnob(grainRandomSlider, grainRandomLabel, "RAND", 0.0, 1.0, 0.01);
    setupGrainKnob(grainArpSlider, grainArpLabel, "ARP", 0.0, 1.0, 0.01);
    setupGrainKnob(grainCloudSlider, grainCloudLabel, "CLOUD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEmitterSlider, grainEmitterLabel, "EMIT", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEnvelopeSlider, grainEnvelopeLabel, "ENV", 0.0, 1.0, 0.01);
    setupGrainKnob(grainShapeSlider, grainShapeLabel, "SHAPE", -1.0, 1.0, 0.01);
    enableAltClickReset(grainSizeSlider, 1240.0);
    enableAltClickReset(grainDensitySlider, 0.05);
    enableAltClickReset(grainPitchSlider, 0.0);
    enableAltClickReset(grainPitchJitterSlider, 0.0);
    enableAltClickReset(grainSpreadSlider, 0.0);
    enableAltClickReset(grainJitterSlider, 0.0);
    enableAltClickReset(grainPositionJitterSlider, 0.0);
    enableAltClickReset(grainRandomSlider, 0.0);
    enableAltClickReset(grainArpSlider, 0.0);
    enableAltClickReset(grainCloudSlider, 0.0);
    enableAltClickReset(grainEmitterSlider, 0.0);
    enableAltClickReset(grainEnvelopeSlider, 0.0);
    enableAltClickReset(grainShapeSlider, 0.0);
    auto setupMini = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };
    setupMini(grainPitchSlider);
    setupMini(grainPitchJitterSlider);
    setupMini(grainSpreadSlider);
    setupMini(grainJitterSlider);
    setupMini(grainPositionJitterSlider);
    setupMini(grainRandomSlider);
    setupMini(grainArpSlider);
    setupMini(grainCloudSlider);
    setupMini(grainEmitterSlider);
    setupMini(grainEnvelopeSlider);
    setupMini(grainShapeSlider);
    grainPitchSlider.textFromValueFunction = [this](double value)
    {
        const bool arpActive = grainArpSlider.getValue() > 0.001;
        const juce::String prefix = arpActive ? "Range " : "Pitch ";
        return prefix + juce::String(value, 1) + " st";
    };
    grainSizeSlider.textFromValueFunction = [this](double value)
    {
        static constexpr std::array<const char*, 13> sizeDivisionLabels {
            "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "1", "2"
        };
        const bool syncEnabled = [this]()
        {
            if (auto* engine = processor.getAudioEngine())
                if (auto* strip = engine->getStrip(stripIndex))
                    return strip->isGrainTempoSyncEnabled();
            return grainSizeSyncToggle.getToggleState();
        }();

        if (!syncEnabled)
            return juce::String(value, 1) + " ms (FREE)";

        const double t = juce::jlimit(0.0, 1.0, (value - 5.0) / (2400.0 - 5.0));
        const int idx = juce::jlimit(0, static_cast<int>(sizeDivisionLabels.size()) - 1,
                                     static_cast<int>(std::round(t * static_cast<double>(sizeDivisionLabels.size() - 1))));
        return juce::String(sizeDivisionLabels[static_cast<size_t>(idx)]);
    };
    grainArpSlider.textFromValueFunction = [](double value)
    {
        if (value <= 0.001)
            return juce::String("Off");
        const int mode = juce::jlimit(0, 5, static_cast<int>(std::floor(juce::jlimit(0.0, 0.999999, value) * 6.0)));
        return getGrainArpModeName(mode);
    };
    grainJitterSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% size jitter";
    };
    grainPositionJitterSlider.textFromValueFunction = [this](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        bool syncEnabled = grainSizeSyncToggle.getToggleState();
        if (auto* engine = processor.getAudioEngine())
            if (auto* strip = engine->getStrip(stripIndex))
                syncEnabled = strip->isGrainTempoSyncEnabled();
        return juce::String(percent) + (syncEnabled ? "% pos jitter (sync)" : "% pos jitter");
    };
    grainRandomSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% macro rand";
    };
    grainEnvelopeSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% Fade";
    };
    grainShapeSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(-1.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% Shape";
    };
    grainPositionJitterSlider.setTooltip("POSJ: grain center position jitter. Quantized to sync grid when Grain SYNC is enabled.");
    grainRandomSlider.setTooltip("RAND: macro random depth (pitch, size, reverse, spray), independent of POSJ.");

    grainSizeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainSizeMs(static_cast<float>(grainSizeSlider.getValue()));
    };
    grainDensitySlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainDensity(static_cast<float>(grainDensitySlider.getValue()));
    };
    grainPitchSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            float value = static_cast<float>(grainPitchSlider.getValue());
            if (strip->getGrainArpDepth() > 0.001f)
            {
                value = std::abs(value);
                if (std::abs(static_cast<float>(grainPitchSlider.getValue()) - value) > 1.0e-4f)
                    grainPitchSlider.setValue(value, juce::dontSendNotification);
            }
            strip->setGrainPitch(value);
        }
    };
    grainPitchJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainPitchJitter(static_cast<float>(grainPitchJitterSlider.getValue()));
    };
    grainSpreadSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainSpread(static_cast<float>(grainSpreadSlider.getValue()));
    };
    grainJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainJitter(static_cast<float>(grainJitterSlider.getValue()));
    };
    grainPositionJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainPositionJitter(static_cast<float>(grainPositionJitterSlider.getValue()));
    };
    grainRandomSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainRandomDepth(static_cast<float>(grainRandomSlider.getValue()));
    };
    grainArpSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainArpDepth(static_cast<float>(grainArpSlider.getValue()));
            if (grainArpSlider.getValue() > 0.001)
            {
                const int mode = juce::jlimit(0, 5, static_cast<int>(std::floor(juce::jlimit(0.0, 0.999999, grainArpSlider.getValue()) * 6.0)));
                strip->setGrainArpMode(mode);
            }
        }
    };
    grainCloudSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainCloudDepth(static_cast<float>(grainCloudSlider.getValue()));
    };
    grainEmitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainEmitterDepth(static_cast<float>(grainEmitterSlider.getValue()));
    };
    grainEnvelopeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainEnvelope(static_cast<float>(grainEnvelopeSlider.getValue()));
    };
    grainShapeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainShape(static_cast<float>(grainShapeSlider.getValue()));
    };

    grainSizeSyncToggle.setButtonText("");
    grainSizeSyncToggle.setClickingTogglesState(true);
    grainSizeSyncToggle.setToggleState(true, juce::dontSendNotification);
    grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickDisabledColourId, stripColor.withAlpha(0.28f));
    grainSizeSyncToggle.setTooltip("Tempo-sync grain size.");
    grainSizeSyncToggle.onClick = [this]()
    {
        const bool enabled = grainSizeSyncToggle.getToggleState();
        grainSizeDivLabel.setText(enabled ? "SYNC" : "FREE", juce::dontSendNotification);
        grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, enabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
        grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, enabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainTempoSyncEnabled(enabled);
    };
    addAndMakeVisible(grainSizeSyncToggle);

    grainSizeDivLabel.setText("SYNC", juce::dontSendNotification);
    grainSizeDivLabel.setJustificationType(juce::Justification::centredRight);
    grainSizeDivLabel.setColour(juce::Label::textColourId, stripColor.withAlpha(0.78f));
    grainSizeDivLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    addAndMakeVisible(grainSizeDivLabel);
    grainSizeLabel.setJustificationType(juce::Justification::centredLeft);

    auto setupGrainTab = [this](juce::TextButton& button, const juce::String& text, GrainSubPage page)
    {
        button.setButtonText(text);
        button.setClickingTogglesState(false);
        button.setTooltip("Grain page: " + text);
        styleUiButton(button, false);
        button.onClick = [this, page]()
        {
            grainSubPage = page;
            updateGrainTabButtons();
            updateGrainOverlayVisibility();
            resized();
            repaint();
        };
        addAndMakeVisible(button);
    };
    setupGrainTab(grainTabPitchButton, "PITCH", GrainSubPage::Pitch);
    setupGrainTab(grainTabSpaceButton, "SPACE", GrainSubPage::Space);
    setupGrainTab(grainTabShapeButton, "SHAPE", GrainSubPage::Shape);
    updateGrainTabButtons();

    patternLengthBox.addItem("16", 1);
    patternLengthBox.addItem("32", 2);
    patternLengthBox.addItem("48", 3);
    patternLengthBox.addItem("64", 4);
    patternLengthBox.setJustificationType(juce::Justification::centred);
    patternLengthBox.setTextWhenNothingSelected("");
    patternLengthBox.setTextWhenNoChoicesAvailable("");
    patternLengthBox.setColour(juce::ComboBox::textColourId, juce::Colours::transparentWhite);
    patternLengthBox.setSelectedId(1, juce::dontSendNotification);
    patternLengthBox.setTooltip("Step pattern length");
    patternLengthBox.onChange = [this]()
    {
        const int bars = juce::jmax(1, patternLengthBox.getSelectedId());
        const int steps = juce::jlimit(1, 64, bars * 16);
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepPatternBars(bars);
        stepLengthReadoutBox.setValue(steps, juce::dontSendNotification);
    };
    addAndMakeVisible(patternLengthBox);

    stepLengthReadoutBox.setRange(1, 64);
    stepLengthReadoutBox.setEditable(false, true, false);
    stepLengthReadoutBox.setJustificationType(juce::Justification::centred);
    stepLengthReadoutBox.setInterceptsMouseClicks(true, false);
    stepLengthReadoutBox.setColour(juce::Label::backgroundColourId, juce::Colour(0xff202427));
    stepLengthReadoutBox.setColour(juce::Label::textColourId, kTextPrimary);
    stepLengthReadoutBox.setColour(juce::Label::outlineColourId, juce::Colour(0xff5a5f64));
    stepLengthReadoutBox.setColour(juce::TextEditor::focusedOutlineColourId, stripColor.withAlpha(0.9f));
    stepLengthReadoutBox.setTooltip("Step pattern length (1..64). Drag or double-click to type.");
    stepLengthReadoutBox.setValue(16, juce::dontSendNotification);
    stepLengthReadoutBox.onValueChange = [this](int steps)
    {
        const int clampedSteps = juce::jlimit(1, 64, steps);
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepPatternLengthSteps(clampedSteps);

        if (clampedSteps % 16 == 0)
            patternLengthBox.setSelectedId(juce::jlimit(1, 4, clampedSteps / 16), juce::dontSendNotification);
        else
        {
            patternLengthBox.setSelectedId(0, juce::dontSendNotification);
            patternLengthBox.setText(juce::String(clampedSteps), juce::dontSendNotification);
        }
    };
    addAndMakeVisible(stepLengthReadoutBox);

    auto setupStepEnvelopeSlider = [this](juce::Slider& slider, juce::Label& label,
                                          const char* text, double min, double max, double def, double skewMid)
    {
        slider.setLookAndFeel(&knobLookAndFeel);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(min, max, 0.1);
        slider.setSkewFactorFromMidPoint(skewMid);
        slider.setValue(def, juce::dontSendNotification);
        slider.setPopupDisplayEnabled(true, false, this);
        slider.setTextValueSuffix(" ms");
        slider.setColour(juce::Slider::trackColourId, stripColor.withAlpha(0.9f));
        slider.setColour(juce::Slider::thumbColourId, stripColor.brighter(0.35f));
        enableAltClickReset(slider, def);
        addAndMakeVisible(slider);

        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, stripColor.brighter(0.35f));
        addAndMakeVisible(label);
    };

    setupStepEnvelopeSlider(stepAttackSlider, stepAttackLabel, "A", 0.0, 400.0, 0.0, 12.0);
    setupStepEnvelopeSlider(stepDecaySlider, stepDecayLabel, "D", 1.0, 4000.0, 4000.0, 700.0);
    setupStepEnvelopeSlider(stepReleaseSlider, stepReleaseLabel, "R", 1.0, 4000.0, 110.0, 180.0);
    stepAttackSlider.setTooltip("Step envelope attack");
    stepDecaySlider.setTooltip("Step envelope decay");
    stepReleaseSlider.setTooltip("Step envelope release");
    stepAttackSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeAttackMs(static_cast<float>(stepAttackSlider.getValue()));
    };
    stepDecaySlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeDecayMs(static_cast<float>(stepDecaySlider.getValue()));
    };
    stepReleaseSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeReleaseMs(static_cast<float>(stepReleaseSlider.getValue()));
    };
    
    // Labels below knobs
    volumeLabel.setText("VOL", juce::dontSendNotification);
    volumeLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));  // Increased from 9
    volumeLabel.setJustificationType(juce::Justification::centred);
    volumeLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(volumeLabel);
    
    panLabel.setText("PAN", juce::dontSendNotification);
    panLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    panLabel.setJustificationType(juce::Justification::centred);
    panLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(panLabel);

    pitchLabel.setText("PITCH", juce::dontSendNotification);
    pitchLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    pitchLabel.setJustificationType(juce::Justification::centred);
    pitchLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(pitchLabel);

    speedLabel.setText("SPEED", juce::dontSendNotification);
    speedLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    speedLabel.setJustificationType(juce::Justification::centred);
    speedLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(speedLabel);
    
    scratchLabel.setText("SCR", juce::dontSendNotification);
    scratchLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    scratchLabel.setJustificationType(juce::Justification::centred);
    scratchLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(scratchLabel);

    sliceLengthLabel.setText("SLICE", juce::dontSendNotification);
    sliceLengthLabel.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
    sliceLengthLabel.setJustificationType(juce::Justification::centred);
    sliceLengthLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(sliceLengthLabel);

    // Label showing current beats setting
    tempoLabel.setText("AUTO", juce::dontSendNotification);
    tempoLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    tempoLabel.setJustificationType(juce::Justification::centred);
    tempoLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(tempoLabel);
    tempoLabel.setTooltip("Beats per loop (auto or manual).");

    recordBarsLabel.setText("", juce::dontSendNotification);
    recordBarsLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    recordBarsLabel.setJustificationType(juce::Justification::centredLeft);
    recordBarsLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(recordBarsLabel);
    recordBarsLabel.setTooltip("Unified loop bars: used for live capture and loaded sample tempo mapping.");

    recordBarsBox.addItem("1/4", 25);
    recordBarsBox.addItem("1/2", 50);
    recordBarsBox.addItem("1", 100);
    recordBarsBox.addItem("2", 200);
    recordBarsBox.addItem("4", 400);
    recordBarsBox.addItem("8", 800);
    recordBarsBox.setJustificationType(juce::Justification::centredLeft);
    recordBarsBox.setSelectedId(100, juce::dontSendNotification);
    recordBarsBox.setTooltip("Loop bars per strip (capture + loaded sample mapping).");
    recordBarsBox.onChange = [this]()
    {
        processor.requestBarLengthChange(stripIndex, recordBarsBox.getSelectedId());
    };
    addAndMakeVisible(recordBarsBox);

    recordButton.setButtonText("REC");
    recordButton.setTooltip("Left-click: capture recent input audio. Right-click: clear recent input buffer.");
    recordButton.onClick = [this]()
    {
        processor.captureRecentAudioToStrip(stripIndex);
    };
    addAndMakeVisible(recordButton);
    recordButton.addMouseListener(this, false);

    modTargetLabel.setText("TARGET", juce::dontSendNotification);
    modTargetLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modTargetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modTargetLabel);

    modTargetBox.addItem("None", 1);
    modTargetBox.addItem("Vol", 2);
    modTargetBox.addItem("Pan", 3);
    modTargetBox.addItem("Pitch", 4);
    modTargetBox.addItem("Speed", 5);
    modTargetBox.addItem("Cutoff", 6);
    modTargetBox.addItem("Reso", 7);
    modTargetBox.addItem("G.Size", 8);
    modTargetBox.addItem("G.Dens", 9);
    modTargetBox.addItem("G.Pitch", 10);
    modTargetBox.addItem("G.PJit", 11);
    modTargetBox.addItem("G.Spread", 12);
    modTargetBox.addItem("G.Jitter", 13);
    modTargetBox.addItem("G.Random", 14);
    modTargetBox.addItem("G.Arp", 15);
    modTargetBox.addItem("G.Cloud", 16);
    modTargetBox.addItem("G.Emit", 17);
    modTargetBox.addItem("G.Env", 18);
    modTargetBox.addItem("Retrig", 19);
    modTargetBox.setSelectedId(1, juce::dontSendNotification);
    modTargetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            engine->setModTarget(stripIndex, comboIdToModTarget(modTargetBox.getSelectedId()));
            modBipolarToggle.setToggleState(engine->isModBipolar(stripIndex), juce::dontSendNotification);
        }
    };
    addAndMakeVisible(modTargetBox);

    modBipolarToggle.setButtonText("BIP");
    modBipolarToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModBipolar(stripIndex, modBipolarToggle.getToggleState());
    };
    addAndMakeVisible(modBipolarToggle);

    modDepthLabel.setText("DEPTH", juce::dontSendNotification);
    modDepthLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modDepthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modDepthLabel);

    modDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modDepthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modDepthSlider.setRange(0.0, 1.0, 0.01);
    modDepthSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModDepth(stripIndex, static_cast<float>(modDepthSlider.getValue()));
    };
    addAndMakeVisible(modDepthSlider);

    modOffsetLabel.setText("SMTH", juce::dontSendNotification);
    modOffsetLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modOffsetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modOffsetLabel);

    modOffsetSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modOffsetSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modOffsetSlider.setRange(0.0, 250.0, 1.0);
    modOffsetSlider.setSkewFactorFromMidPoint(40.0);
    modOffsetSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModSmoothingMs(stripIndex, static_cast<float>(modOffsetSlider.getValue()));
    };
    addAndMakeVisible(modOffsetSlider);

    modCurveBendLabel.setText("BEND", juce::dontSendNotification);
    modCurveBendLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modCurveBendLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modCurveBendLabel);

    modCurveBendSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modCurveBendSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modCurveBendSlider.setRange(-1.0, 1.0, 0.01);
    modCurveBendSlider.setValue(0.0, juce::dontSendNotification);
    modCurveBendSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveBend(stripIndex, static_cast<float>(modCurveBendSlider.getValue()));
    };
    addAndMakeVisible(modCurveBendSlider);

    modLengthLabel.setText("LEN", juce::dontSendNotification);
    modLengthLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modLengthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modLengthLabel);

    modLengthBox.addItem("1", 1);
    modLengthBox.addItem("2", 2);
    modLengthBox.addItem("4", 4);
    modLengthBox.addItem("8", 8);
    modLengthBox.setSelectedId(1, juce::dontSendNotification);
    modLengthBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModLengthBars(stripIndex, modLengthBox.getSelectedId());
    };
    addAndMakeVisible(modLengthBox);

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& tab = modSequencerTabs[static_cast<size_t>(slot)];
        tab.setButtonText("S" + juce::String(slot + 1));
        tab.setTooltip("Switch to modulation sequencer " + juce::String(slot + 1) + ".");
        tab.onClick = [this, slot]()
        {
            if (auto* engine = processor.getAudioEngine())
                engine->setModSequencerSlot(stripIndex, slot);
            updateModSequencerTabButtons();
            repaint();
        };
        addAndMakeVisible(tab);
    }
    updateModSequencerTabButtons();

    modPitchQuantToggle.setButtonText("P.Quant");
    modPitchQuantToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScaleQuantize(stripIndex, modPitchQuantToggle.getToggleState());
    };
    addAndMakeVisible(modPitchQuantToggle);

    modPitchScaleBox.addItem("Chrom", 1);
    modPitchScaleBox.addItem("Maj", 2);
    modPitchScaleBox.addItem("Min", 3);
    modPitchScaleBox.addItem("Dor", 4);
    modPitchScaleBox.addItem("Pent", 5);
    modPitchScaleBox.setSelectedId(1, juce::dontSendNotification);
    modPitchScaleBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScale(stripIndex, comboIdToPitchScale(modPitchScaleBox.getSelectedId()));
    };
    addAndMakeVisible(modPitchScaleBox);

    modShapeLabel.setText("SHAPE", juce::dontSendNotification);
    modShapeLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modShapeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modShapeLabel);

    modShapeBox.addItem("Curve", 1);
    modShapeBox.addItem("Steps", 2);
    modShapeBox.setSelectedId(1, juce::dontSendNotification);
    modShapeBox.onChange = [this]()
    {
        const bool curveMode = (modShapeBox.getSelectedId() == 1);
        modCurveBendSlider.setEnabled(curveMode);
        modCurveTypeBox.setEnabled(curveMode);
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveMode(stripIndex, curveMode);
    };
    addAndMakeVisible(modShapeBox);

    modCurveTypeLabel.setText("CTYPE", juce::dontSendNotification);
    modCurveTypeLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modCurveTypeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modCurveTypeLabel);

    modCurveTypeBox.addItem("Normal", 1);
    modCurveTypeBox.addItem("Exp+", 2);
    modCurveTypeBox.addItem("Exp-", 3);
    modCurveTypeBox.addItem("Sine", 4);
    modCurveTypeBox.addItem("Square", 5);
    modCurveTypeBox.setSelectedId(1, juce::dontSendNotification);
    modCurveTypeBox.setTooltip("Curve draw type in Curve mode: Normal, Exp+/-, Sine, Square.");
    modCurveTypeBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveShape(stripIndex, comboIdToCurveShape(modCurveTypeBox.getSelectedId()));
    };
    addAndMakeVisible(modCurveTypeBox);
    
    // Legacy readout removed from strip UI (kept as hidden component for compatibility).
    recordLengthLabel.setVisible(false);

    patternLengthBox.setVisible(false);
    stepLengthReadoutBox.setVisible(false);
    stepAttackSlider.setVisible(false);
    stepDecaySlider.setVisible(false);
    stepReleaseSlider.setVisible(false);
    stepAttackLabel.setVisible(false);
    stepDecayLabel.setVisible(false);
    stepReleaseLabel.setVisible(false);
    updateGrainOverlayVisibility();
}

void StripControl::updateGrainOverlayVisibility()
{
    const bool isGrainMode = !showingStepDisplay
                          && processor.getAudioEngine()
                          && processor.getAudioEngine()->getStrip(stripIndex)
                          && processor.getAudioEngine()->getStrip(stripIndex)->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain;
    const bool isStepMode = showingStepDisplay;
    grainOverlayVisible = isGrainMode;
    const bool showPitchPage = isGrainMode && grainSubPage == GrainSubPage::Pitch;
    const bool showSpacePage = isGrainMode && grainSubPage == GrainSubPage::Space;
    const bool showShapePage = isGrainMode && grainSubPage == GrainSubPage::Shape;

    volumeSlider.setVisible(!isGrainMode);
    panSlider.setVisible(!isGrainMode);
    volumeLabel.setVisible(!isGrainMode);
    panLabel.setVisible(!isGrainMode);

    const bool showLoopKnobs = !showingStepDisplay && !isGrainMode;
    bool showSliceLength = false;
    if (showLoopKnobs && processor.getAudioEngine())
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            showSliceLength = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Loop);
    }

    pitchSlider.setVisible(showLoopKnobs);
    speedSlider.setVisible(showLoopKnobs);
    scratchSlider.setVisible(showLoopKnobs);
    sliceLengthSlider.setVisible(showSliceLength);
    pitchLabel.setVisible(showLoopKnobs);
    speedLabel.setVisible(showLoopKnobs);
    scratchLabel.setVisible(showLoopKnobs);
    sliceLengthLabel.setVisible(showSliceLength);
    patternLengthBox.setVisible(isStepMode && !isGrainMode);
    stepLengthReadoutBox.setVisible(isStepMode && !isGrainMode);
    stepAttackSlider.setVisible(isStepMode && !isGrainMode);
    stepDecaySlider.setVisible(isStepMode && !isGrainMode);
    stepReleaseSlider.setVisible(isStepMode && !isGrainMode);
    stepAttackLabel.setVisible(isStepMode && !isGrainMode);
    stepDecayLabel.setVisible(isStepMode && !isGrainMode);
    stepReleaseLabel.setVisible(isStepMode && !isGrainMode);
    recordLengthLabel.setVisible(false);

    grainSizeSlider.setVisible(isGrainMode);
    grainDensitySlider.setVisible(isGrainMode);
    grainPitchSlider.setVisible(showPitchPage);
    grainPitchJitterSlider.setVisible(showPitchPage);
    grainSpreadSlider.setVisible(showSpacePage);
    grainJitterSlider.setVisible(showSpacePage);
    grainPositionJitterSlider.setVisible(showSpacePage);
    grainRandomSlider.setVisible(showPitchPage);
    grainArpSlider.setVisible(showPitchPage);
    grainCloudSlider.setVisible(showSpacePage);
    grainEmitterSlider.setVisible(showSpacePage);
    grainEnvelopeSlider.setVisible(showShapePage);
    grainShapeSlider.setVisible(showShapePage);
    grainTabPitchButton.setVisible(isGrainMode);
    grainTabSpaceButton.setVisible(isGrainMode);
    grainTabShapeButton.setVisible(isGrainMode);
    grainSizeSyncToggle.setVisible(isGrainMode);
    grainSizeDivLabel.setVisible(isGrainMode);
    grainSizeLabel.setVisible(isGrainMode);
    grainDensityLabel.setVisible(isGrainMode);
    grainPitchLabel.setVisible(showPitchPage);
    grainPitchJitterLabel.setVisible(showPitchPage);
    grainSpreadLabel.setVisible(showSpacePage);
    grainJitterLabel.setVisible(showSpacePage);
    grainPositionJitterLabel.setVisible(showSpacePage);
    grainRandomLabel.setVisible(showPitchPage);
    grainArpLabel.setVisible(showPitchPage);
    grainCloudLabel.setVisible(showSpacePage);
    grainEmitterLabel.setVisible(showSpacePage);
    grainEnvelopeLabel.setVisible(showShapePage);
    grainShapeLabel.setVisible(showShapePage);
    updateGrainTabButtons();
}

void StripControl::updateGrainTabButtons()
{
    auto tintTab = [](juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId,
                         active ? kAccent.withAlpha(0.95f) : juce::Colour(0xff3b4146));
        button.setColour(juce::TextButton::buttonOnColourId,
                         active ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
        button.setColour(juce::TextButton::textColourOffId,
                         active ? juce::Colour(0xff121212) : kTextPrimary);
        button.setColour(juce::TextButton::textColourOnId,
                         active ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
    };
    tintTab(grainTabPitchButton, grainSubPage == GrainSubPage::Pitch);
    tintTab(grainTabSpaceButton, grainSubPage == GrainSubPage::Space);
    tintTab(grainTabShapeButton, grainSubPage == GrainSubPage::Shape);
}

void StripControl::updateModSequencerTabButtons()
{
    const int activeSlot = processor.getAudioEngine()
        ? processor.getAudioEngine()->getModSequencerSlot(stripIndex)
        : 0;

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& tab = modSequencerTabs[static_cast<size_t>(slot)];
        const bool active = (slot == activeSlot);
        tab.setColour(juce::TextButton::buttonColourId,
                      active ? kAccent.withAlpha(0.95f) : juce::Colour(0xff3b4146));
        tab.setColour(juce::TextButton::buttonOnColourId,
                      active ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
        tab.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colour(0xff121212) : kTextPrimary);
        tab.setColour(juce::TextButton::textColourOnId,
                      active ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
    }
}


void StripControl::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    drawPanel(g, bounds, stripColor, 10.0f);

    if (modulationLaneView)
    {
        paintModulationLane(g);
    }
    else
    {
        // Paint LED overlay on top of waveform area
        paintLEDOverlay(g);
    }

}

void StripControl::setModulationLaneView(bool shouldShow)
{
    if (modulationLaneView == shouldShow)
        return;
    if (shouldShow)
    {
        preModulationShowingStepDisplay = showingStepDisplay;
        preModulationWaveformVisible = waveform.isVisible();
        preModulationStepVisible = stepDisplay.isVisible();
    }
    modulationLaneView = shouldShow;
    if (!shouldShow)
    {
        showingStepDisplay = preModulationShowingStepDisplay;
        waveform.setVisible(preModulationWaveformVisible);
        stepDisplay.setVisible(preModulationStepVisible);
        modulationLastDrawStep = -1;
        updateGrainOverlayVisibility();
        updateFromEngine();
    }
    resized();
    repaint();
}

juce::Rectangle<int> StripControl::getModulationLaneBounds() const
{
    return modulationLaneBounds;
}

void StripControl::paintModulationLane(juce::Graphics& g)
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    auto lane = getModulationLaneBounds();
    if (lane.isEmpty())
        return;

    const auto seq = engine->getModSequencerState(stripIndex);
    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    const int activeStep = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
    if (totalSteps <= 0)
        return;

    g.setColour(juce::Colour(0xff1f1f1f));
    g.fillRoundedRectangle(lane.toFloat(), 6.0f);
    g.setColour(stripColor.withAlpha(0.35f));
    g.drawRoundedRectangle(lane.toFloat().reduced(0.5f), 6.0f, 1.0f);

    const int activeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, engine->getModSequencerSlot(stripIndex));
    const juce::String laneInfo = "SEQ " + juce::String(activeSlot + 1)
        + "  PAGE " + juce::String(seq.editPage + 1) + "/" + juce::String(lengthBars);
    auto infoBadge = juce::Rectangle<float>(lane.getX() + 8.0f, lane.getY() + 4.0f, 118.0f, 14.0f);
    g.setColour(juce::Colour(0xff111111).withAlpha(0.72f));
    g.fillRoundedRectangle(infoBadge, 3.0f);
    g.setColour(stripColor.withAlpha(0.22f));
    g.drawRoundedRectangle(infoBadge, 3.0f, 1.0f);
    g.setColour(kTextPrimary.withAlpha(0.88f));
    g.setFont(8.5f);
    g.drawText(laneInfo, infoBadge.toNearestInt(), juce::Justification::centred, false);

    const auto drawLane = lane.reduced(12, 2);
    const float dotSize = (totalSteps > 32) ? 4.0f : 6.0f;
    const float dotPad = dotSize * 0.6f;
    const float left = static_cast<float>(drawLane.getX()) + dotPad;
    const float right = juce::jmax(left, static_cast<float>(drawLane.getRight() - 1) - dotPad);
    const float top = static_cast<float>(drawLane.getY()) + 2.0f;
    const float bottom = static_cast<float>(drawLane.getBottom()) - 2.0f;
    const float width = juce::jmax(1.0f, right - left);
    const float height = bottom - top;
    const float stepWidth = juce::jmax(0.25f, width / static_cast<float>(juce::jmax(1, totalSteps)));
    const float centerY = top + (height * 0.5f);

    if (seq.bipolar)
    {
        g.setColour(juce::Colour(0xff454545));
        g.drawLine(left, centerY, right, centerY, 1.0f);
    }

    auto valueToY = [&](float v) -> float
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        const float n = seq.bipolar ? ((clamped * 2.0f) - 1.0f) : clamped;
        return seq.bipolar
            ? (centerY - (n * (height * 0.48f)))
            : (bottom - (n * height));
    };

    std::vector<float> startValues(static_cast<size_t>(totalSteps));
    std::vector<float> endValues(static_cast<size_t>(totalSteps));
    std::vector<int> subdivisions(static_cast<size_t>(totalSteps));
    std::vector<ModernAudioEngine::ModCurveShape> stepCurveShapes(static_cast<size_t>(totalSteps),
                                                                   ModernAudioEngine::ModCurveShape::Linear);
    for (int i = 0; i < totalSteps; ++i)
    {
        const float startValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, i));
        const int subdiv = juce::jlimit(
            1, ModernAudioEngine::ModMaxStepSubdivisions, engine->getModStepSubdivisionAbsolute(stripIndex, i));
        float endValue = juce::jlimit(0.0f, 1.0f, engine->getModStepEndValueAbsolute(stripIndex, i));
        const auto stepCurveShape = engine->getModStepCurveShapeAbsolute(stripIndex, i);
        if (subdiv <= 1)
            endValue = startValue;
        startValues[static_cast<size_t>(i)] = startValue;
        endValues[static_cast<size_t>(i)] = endValue;
        subdivisions[static_cast<size_t>(i)] = subdiv;
        stepCurveShapes[static_cast<size_t>(i)] = stepCurveShape;
    }

    const float activeStepX = left + (stepWidth * static_cast<float>(activeStep));
    g.setColour(kAccent.withAlpha(0.10f));
    g.fillRect(activeStepX, top, juce::jmax(1.0f, stepWidth), juce::jmax(1.0f, height));

    const float bend = juce::jlimit(-1.0f, 1.0f, seq.curveBend);
    std::vector<juce::Point<float>> stepMarkerPoints(static_cast<size_t>(totalSteps));
    for (int i = 0; i < totalSteps; ++i)
    {
        const float markerPhase = seq.curveMode ? shapeSubdivisionBendPhaseUi(0.5f, bend) : 0.5f;
        const float markerValue = (subdivisions[static_cast<size_t>(i)] > 1)
            ? sampleModSubdivisionValueUi(
                startValues[static_cast<size_t>(i)],
                endValues[static_cast<size_t>(i)],
                subdivisions[static_cast<size_t>(i)],
                markerPhase)
            : startValues[static_cast<size_t>(i)];
        const float x = left + (stepWidth * (static_cast<float>(i) + 0.5f));
        stepMarkerPoints[static_cast<size_t>(i)] = { x, valueToY(markerValue) };
    }

    if (seq.curveMode)
    {
        juce::Path rawPath;
        std::vector<float> sampledX;
        std::vector<float> sampledValues;
        sampledX.reserve(static_cast<size_t>(totalSteps * 10));
        sampledValues.reserve(static_cast<size_t>(totalSteps * 10));
        bool started = false;
        for (int i = 0; i < totalSteps; ++i)
        {
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const bool hasLocalRamp = (subdiv > 1);
            const int segmentCount = hasLocalRamp ? juce::jlimit(2, 64, subdiv * 4) : 8;
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float nextStart = startValues[static_cast<size_t>((i + 1) % totalSteps)];

            for (int s = 0; s <= segmentCount; ++s)
            {
                if (i > 0 && s == 0)
                    continue;

                const float t = static_cast<float>(s) / static_cast<float>(segmentCount);
                const float shapedT = shapeCurvePhaseUi(
                    t,
                    bend,
                    stepCurveShapes[static_cast<size_t>(i)]);
                const float bendT = shapeSubdivisionBendPhaseUi(t, bend);
                const float value = hasLocalRamp
                    ? sampleModSubdivisionValueUi(startValue,
                                                  endValue,
                                                  subdiv,
                                                  bendT)
                    : juce::jlimit(0.0f, 1.0f,
                                   startValue + ((nextStart - startValue) * shapedT));
                const float x = juce::jlimit(left, right, left + (stepWidth * (static_cast<float>(i) + t)));
                const float y = valueToY(value);

                if (!started)
                {
                    rawPath.startNewSubPath(x, y);
                    started = true;
                }
                else
                {
                    rawPath.lineTo(x, y);
                }
                sampledX.push_back(x);
                sampledValues.push_back(value);
            }
        }

        const float smoothingMs = juce::jlimit(0.0f, 250.0f, seq.smoothingMs);
        const bool showSmoothedOverlay = (smoothingMs > 0.05f && sampledValues.size() > 2);

        g.setColour(stripColor.withAlpha(showSmoothedOverlay ? 0.58f : 0.9f));
        g.strokePath(rawPath, juce::PathStrokeType(showSmoothedOverlay ? 1.6f : 2.0f));

        if (showSmoothedOverlay)
        {
            // Approximate post-curve smoothing for visual feedback in curve mode.
            const float refStepMs = 125.0f;
            const float totalMs = refStepMs * static_cast<float>(totalSteps);
            const int sampleCount = static_cast<int>(sampledValues.size());
            const float dtMs = totalMs / static_cast<float>(juce::jmax(1, sampleCount - 1));
            const float alpha = 1.0f - std::exp(-dtMs / juce::jmax(1.0f, smoothingMs));

            float smoothed = sampledValues.front();
            juce::Path smoothPath;
            smoothPath.startNewSubPath(sampledX.front(), valueToY(smoothed));
            for (size_t idx = 1; idx < sampledValues.size(); ++idx)
            {
                smoothed += (sampledValues[idx] - smoothed) * juce::jlimit(0.0f, 1.0f, alpha);
                smoothPath.lineTo(sampledX[idx], valueToY(smoothed));
            }

            g.setColour(juce::Colour(0xff101010).withAlpha(0.68f));
            g.strokePath(smoothPath, juce::PathStrokeType(3.4f));
            g.setColour(kAccent.brighter(0.35f).withAlpha(0.92f));
            g.strokePath(smoothPath, juce::PathStrokeType(2.2f));

            auto badge = juce::Rectangle<float>(right - 76.0f, top + 1.0f, 74.0f, 13.0f);
            g.setColour(juce::Colour(0xff111111).withAlpha(0.74f));
            g.fillRoundedRectangle(badge, 3.0f);
            g.setColour(kAccent.withAlpha(0.26f));
            g.drawRoundedRectangle(badge, 3.0f, 1.0f);
            g.setColour(juce::Colour(0xfff8e7c2).withAlpha(0.92f));
            g.setFont(8.0f);
            g.drawText("Smth " + juce::String(static_cast<int>(std::round(smoothingMs))) + "ms",
                       badge.toNearestInt(), juce::Justification::centred, false);
        }
    }
    else
    {
        for (int i = 0; i < totalSteps; ++i)
        {
            const float stepX = left + (stepWidth * static_cast<float>(i));
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float slotWidth = stepWidth / static_cast<float>(juce::jmax(1, subdiv));
            const float barWidth = juce::jmax(1.0f, slotWidth * 0.72f);

            for (int s = 0; s < subdiv; ++s)
            {
                const float t = (subdiv <= 1)
                    ? 1.0f
                    : (static_cast<float>(s + 1) / static_cast<float>(subdiv));
                const float value = (subdiv <= 1)
                    ? startValue
                    : juce::jlimit(0.0f, 1.0f, startValue + ((endValue - startValue) * t));

                float y0 = seq.bipolar ? centerY : bottom;
                const float y1 = valueToY(value);
                const float x = stepX + (slotWidth * (static_cast<float>(s) + 0.5f)) - (barWidth * 0.5f);
                const float yTop = juce::jmin(y0, y1);
                const float h = juce::jmax(1.0f, std::abs(y1 - y0));
                const float shade = (subdiv <= 1)
                    ? 0.55f
                    : juce::jmap(static_cast<float>(s) / static_cast<float>(juce::jmax(1, subdiv - 1)), 0.72f, 0.44f);
                g.setColour(stripColor.withAlpha(shade));
                g.fillRoundedRectangle(x, yTop, barWidth, h, 1.5f);
            }
        }
    }

    for (int i = 0; i < totalSteps; ++i)
    {
        const auto point = stepMarkerPoints[static_cast<size_t>(i)];
        const bool isActive = (i == activeStep);
        g.setColour(isActive ? kAccent : stripColor.withMultipliedBrightness(0.8f));
        g.fillEllipse(point.x - (dotSize * 0.5f), point.y - (dotSize * 0.5f), dotSize, dotSize);

        const int subdiv = subdivisions[static_cast<size_t>(i)];
        if (subdiv > 1 && stepWidth > 14.0f)
        {
            auto label = juce::Rectangle<float>(
                left + (stepWidth * static_cast<float>(i)),
                top + 1.0f,
                juce::jmax(8.0f, stepWidth),
                juce::jmax(8.0f, juce::jmin(12.0f, height * 0.25f)));
            g.setColour(juce::Colour(0xfff0f0f0).withAlpha(0.72f));
            g.setFont(juce::jmax(7.0f, juce::jmin(10.0f, stepWidth * 0.34f)));
            g.drawText("x" + juce::String(subdiv), label.toNearestInt(), juce::Justification::centred, false);
        }
    }
}

void StripControl::applyModulationPoint(juce::Point<int> p)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return;

    auto lane = getModulationLaneBounds().reduced(12, 2);
    auto hitLane = lane.expanded(1, 0);
    if (!hitLane.contains(p))
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modulationLastDrawStep >= totalSteps)
        modulationLastDrawStep = -1;
    const float x = juce::jlimit(static_cast<float>(lane.getX()),
                                 static_cast<float>(lane.getRight() - 1),
                                 static_cast<float>(p.x));
    const float nx = juce::jlimit(0.0f, 1.0f, (x - static_cast<float>(lane.getX())) / juce::jmax(1.0f, static_cast<float>(lane.getWidth() - 1)));
    const float ny = juce::jlimit(0.0f, 1.0f, (static_cast<float>(p.y - lane.getY())) / juce::jmax(1.0f, static_cast<float>(lane.getHeight())));
    const int step = juce::jlimit(0, totalSteps - 1,
                                  static_cast<int>(std::round(nx * static_cast<float>(juce::jmax(1, totalSteps - 1)))));
    const float value = juce::jlimit(0.0f, 1.0f, 1.0f - ny);
    if (modulationLastDrawStep < 0)
    {
        engine->setModStepValueAbsolute(stripIndex, step, value);
        modulationLastDrawStep = step;
        modulationLastDrawValue = value;
        return;
    }

    const int from = juce::jmin(modulationLastDrawStep, step);
    const int to = juce::jmax(modulationLastDrawStep, step);
    for (int s = from; s <= to; ++s)
    {
        const float t = (to == from) ? 1.0f : (static_cast<float>(s - from) / static_cast<float>(to - from));
        const float v = modulationLastDrawValue + ((value - modulationLastDrawValue) * t);
        engine->setModStepValueAbsolute(stripIndex, s, v);
    }
    modulationLastDrawStep = step;
    modulationLastDrawValue = value;
}

int StripControl::getModulationStepFromPoint(juce::Point<int> p) const
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return -1;
    auto lane = getModulationLaneBounds().reduced(12, 2);
    if (lane.isEmpty())
        return -1;
    if (!lane.expanded(1, 0).contains(p))
        return -1;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    const float x = juce::jlimit(static_cast<float>(lane.getX()),
                                 static_cast<float>(lane.getRight() - 1),
                                 static_cast<float>(p.x));
    const float nx = juce::jlimit(0.0f, 1.0f,
                                  (x - static_cast<float>(lane.getX()))
                                  / juce::jmax(1.0f, static_cast<float>(lane.getWidth() - 1)));
    return juce::jlimit(0, totalSteps - 1,
                        static_cast<int>(std::round(nx * static_cast<float>(juce::jmax(1, totalSteps - 1)))));
}

void StripControl::applyModulationCellDuplicateFromDrag(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips || modTransformStep < 0)
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modTransformStep >= totalSteps)
        return;

    const int nextSubdivision = juce::jlimit(
        1,
        ModernAudioEngine::ModMaxStepSubdivisions,
        modTransformStartSubdivision + ((-deltaY) / 14));
    const float endValue = (nextSubdivision > 1) ? modTransformStartEndValue : modTransformStartValue;
    engine->setModStepShapeAbsolute(stripIndex, modTransformStep, nextSubdivision, endValue);
}

void StripControl::applyModulationCellCurveFromDrag(int deltaY, bool rampUpMode)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips || modTransformStep < 0)
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modTransformStep >= totalSteps)
        return;

    int subdivisions = modTransformStartSubdivision;
    if (subdivisions <= 1)
    {
        subdivisions = juce::jlimit(
            2,
            ModernAudioEngine::ModMaxStepSubdivisions,
            2 + (std::abs(deltaY) / 14));
    }

    float startValue = modTransformStartValue;
    float endValue = modTransformStartEndValue;
    computeSingleModCellRamp(modTransformStartValue, modTransformStartEndValue, deltaY, rampUpMode, startValue, endValue);
    engine->setModStepValueAbsolute(stripIndex, modTransformStep, startValue);
    engine->setModStepShapeAbsolute(stripIndex, modTransformStep, subdivisions, endValue);
}

void StripControl::mouseDown(const juce::MouseEvent& e)
{
    if (e.originalComponent == &recordButton || e.eventComponent == &recordButton)
    {
        if (e.mods.isRightButtonDown())
            processor.clearRecentAudioBuffer();
        return;
    }

    if (modulationLaneView)
    {
        auto* engine = processor.getAudioEngine();
        if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
            return;

        const auto state = engine->getModSequencerState(stripIndex);
        const bool neutralBipolar = modTargetAllowsBipolar(state.target) && state.bipolar;
        const float neutralValue = neutralBipolar ? 0.5f : 0.0f;
        const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
        const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);

        const auto mods = e.mods;
        const auto modifierGesture = getStepCellModifierGesture(mods);
        const int clickedStep = getModulationStepFromPoint(e.getPosition());
        if (clickedStep >= 0 && modifierGesture != StepCellModifierGesture::None)
        {
            modTransformStartY = e.y;
            modTransformStep = clickedStep;
            modTransformStartValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, clickedStep));
            modTransformStartSubdivision = juce::jlimit(
                1,
                ModernAudioEngine::ModMaxStepSubdivisions,
                engine->getModStepSubdivisionAbsolute(stripIndex, clickedStep));
            modTransformStartEndValue = juce::jlimit(
                0.0f, 1.0f, engine->getModStepEndValueAbsolute(stripIndex, clickedStep));
            switch (modifierGesture)
            {
                case StepCellModifierGesture::Divide:
                    modTransformMode = ModTransformMode::DuplicateCell;
                    break;
                case StepCellModifierGesture::RampUp:
                    modTransformMode = ModTransformMode::ShapeUpCell;
                    break;
                case StepCellModifierGesture::RampDown:
                    modTransformMode = ModTransformMode::ShapeDownCell;
                    break;
                case StepCellModifierGesture::None:
                default:
                    modTransformMode = ModTransformMode::None;
                    break;
            }

            if (modTransformMode == ModTransformMode::ShapeUpCell)
                applyModulationCellCurveFromDrag(0, true);
            else if (modTransformMode == ModTransformMode::ShapeDownCell)
                applyModulationCellCurveFromDrag(0, false);
            return;
        }

        if (mods.isRightButtonDown() && modifierGesture == StepCellModifierGesture::None)
        {
            for (int i = 0; i < totalSteps; ++i)
                engine->setModStepValueAbsolute(stripIndex, i, neutralValue);
            modulationLastDrawStep = -1;
            return;
        }

        modTransformMode = ModTransformMode::None;
        modTransformStep = -1;
        modulationLastDrawStep = -1;
        applyModulationPoint(e.getPosition());
    }
}

void StripControl::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!modulationLaneView)
        return;

    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return;

    const int step = getModulationStepFromPoint(e.getPosition());
    if (step < 0)
        return;

    const auto state = engine->getModSequencerState(stripIndex);
    const bool neutralBipolar = modTargetAllowsBipolar(state.target) && state.bipolar;
    const float neutralValue = neutralBipolar ? 0.5f : 0.0f;
    engine->setModStepValueAbsolute(stripIndex, step, neutralValue);
    modulationLastDrawStep = -1;
}

void StripControl::mouseDrag(const juce::MouseEvent& e)
{
    if (modulationLaneView)
    {
        if (modTransformMode != ModTransformMode::None)
        {
            const int deltaY = e.y - modTransformStartY;
            if (modTransformMode == ModTransformMode::DuplicateCell)
                applyModulationCellDuplicateFromDrag(deltaY);
            else if (modTransformMode == ModTransformMode::ShapeUpCell)
                applyModulationCellCurveFromDrag(deltaY, true);
            else if (modTransformMode == ModTransformMode::ShapeDownCell)
                applyModulationCellCurveFromDrag(deltaY, false);
            return;
        }

        applyModulationPoint(e.getPosition());
    }
}

void StripControl::mouseUp(const juce::MouseEvent&)
{
    modTransformMode = ModTransformMode::None;
    modTransformStep = -1;
}

void StripControl::hideAllPrimaryControls()
{
    auto hide = [](juce::Component& c){ c.setVisible(false); };
    hide(loadButton); hide(transientSliceButton); hide(playModeBox); hide(directionModeBox); hide(groupSelector);
    hide(volumeSlider); hide(panSlider); hide(pitchSlider); hide(speedSlider); hide(scratchSlider); hide(sliceLengthSlider); hide(patternLengthBox); hide(stepLengthReadoutBox);
    hide(stepAttackSlider); hide(stepDecaySlider); hide(stepReleaseSlider);
    hide(tempoLabel); hide(recordBarsBox); hide(recordButton); hide(recordBarsLabel);
    hide(volumeLabel); hide(panLabel); hide(pitchLabel); hide(speedLabel); hide(scratchLabel); hide(sliceLengthLabel);
    hide(stepAttackLabel); hide(stepDecayLabel); hide(stepReleaseLabel);
    hide(recordLengthLabel);
}

void StripControl::hideAllGrainControls()
{
    auto hide = [](juce::Component& c){ c.setVisible(false); };
    hide(grainSizeSlider); hide(grainDensitySlider); hide(grainPitchSlider); hide(grainPitchJitterSlider);
    hide(grainSpreadSlider); hide(grainJitterSlider); hide(grainPositionJitterSlider); hide(grainRandomSlider); hide(grainArpSlider);
    hide(grainCloudSlider); hide(grainEmitterSlider); hide(grainEnvelopeSlider); hide(grainShapeSlider);
    hide(grainTabPitchButton); hide(grainTabSpaceButton); hide(grainTabShapeButton);
    hide(grainSizeSyncToggle); hide(grainSizeDivLabel); hide(grainSizeLabel);
    hide(grainDensityLabel); hide(grainPitchLabel); hide(grainPitchJitterLabel); hide(grainSpreadLabel);
    hide(grainJitterLabel); hide(grainPositionJitterLabel); hide(grainRandomLabel); hide(grainArpLabel); hide(grainCloudLabel);
    hide(grainEmitterLabel); hide(grainEnvelopeLabel); hide(grainShapeLabel);
}

void StripControl::paintLEDOverlay(juce::Graphics& g)
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip || !strip->hasAudio()) return;
    
    // Get waveform bounds
    auto wfBounds = waveform.getBounds();
    if (wfBounds.isEmpty() || wfBounds.getWidth() <= 0 || wfBounds.getHeight() <= 0) 
        return;
    
    float colWidth = wfBounds.getWidth() / 16.0f;
    float ledHeight = 10.0f;
    
    // Safety check for valid dimensions
    if (!std::isfinite(colWidth) || colWidth <= 0.0f || ledHeight <= 0.0f)
        return;
    
    int currentCol = strip->getCurrentColumn();
    int loopStart = strip->getLoopStart();
    int loopEnd = strip->getLoopEnd();
    bool isPlaying = strip->isPlaying();
    
    // Draw LED blocks at top of waveform
    for (int x = 0; x < 16; ++x)
    {
        float xPos = wfBounds.getX() + x * colWidth;
        float rectWidth = colWidth - 2.0f;
        
        // Validate rectangle dimensions
        if (!std::isfinite(xPos) || !std::isfinite(rectWidth) || rectWidth <= 0.0f)
            continue;
        
        juce::Rectangle<float> ledRect(xPos + 1.0f, wfBounds.getY() + 1.0f, rectWidth, ledHeight);
        
        // Double-check the rectangle is valid
        if (ledRect.isEmpty() || !ledRect.isFinite())
            continue;
        
        // Determine LED brightness
        juce::Colour ledColor;
        
        if (isPlaying && x == currentCol)
        {
            ledColor = kAccent;
        }
        else if (x >= loopStart && x < loopEnd)
        {
            ledColor = juce::Colour(0xff4f4f4f);
        }
        else
        {
            ledColor = juce::Colour(0xff292929);
        }
        
        g.setColour(ledColor);
        g.fillRoundedRectangle(ledRect, 1.0f);
        
        // Subtle border
        g.setColour(juce::Colour(0xff171717));
        g.drawRoundedRectangle(ledRect, 1.0f, 0.5f);
    }
}

void StripControl::resized()
{
    auto bounds = getLocalBounds().reduced(2);
    
    // Safety check for minimum size
    if (bounds.getWidth() < 50 || bounds.getHeight() < 50)
        return;
    
    // Label at very top
    auto labelArea = bounds.removeFromTop(14);
    stripLabel.setBounds(labelArea.removeFromLeft(30));
    
    // Main area splits: waveform left, controls right
    auto controlsArea = bounds.removeFromRight(228);
    
    // Waveform OR step display gets all remaining space
    waveform.setBounds(bounds);
    stepDisplay.setBounds(bounds);  // Same position, visibility toggled
    modulationLaneBounds = bounds;  // Match waveform/step display exactly
    
    if (modulationLaneView)
    {
        waveform.setVisible(false);
        stepDisplay.setVisible(false);
        hideAllPrimaryControls();
        hideAllGrainControls();

        modTargetLabel.setVisible(true);
        modTargetBox.setVisible(true);
        modBipolarToggle.setVisible(true);
        modDepthLabel.setVisible(true);
        modDepthSlider.setVisible(true);
        modOffsetLabel.setVisible(true);
        modOffsetSlider.setVisible(true);
        modCurveBendLabel.setVisible(true);
        modCurveBendSlider.setVisible(true);
        modLengthLabel.setVisible(true);
        modLengthBox.setVisible(true);
        for (auto& tab : modSequencerTabs)
            tab.setVisible(true);
        modPitchQuantToggle.setVisible(true);
        modPitchScaleBox.setVisible(true);
        modShapeLabel.setVisible(true);
        modShapeBox.setVisible(true);
        modCurveTypeLabel.setVisible(true);
        modCurveTypeBox.setVisible(true);

        controlsArea.reduce(4, 0);
        const int gap = 4;
        const int compactGap = 1;
        const int tabRowHeight = 14;
        const int compactRowHeight = 16;
        const int columnWidth = juce::jmax(80, (controlsArea.getWidth() - gap) / 2);
        auto splitRow = [columnWidth](juce::Rectangle<int> row, const int rowGap)
        {
            auto left = row.removeFromLeft(columnWidth);
            row.removeFromLeft(rowGap);
            return std::pair<juce::Rectangle<int>, juce::Rectangle<int>>(left, row);
        };

        auto tabRow = controlsArea.removeFromTop(tabRowHeight);
        const int tabGap = 2;
        const int tabWidth = juce::jmax(1, (tabRow.getWidth() - ((ModernAudioEngine::NumModSequencers - 1) * tabGap))
                                             / ModernAudioEngine::NumModSequencers);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            modSequencerTabs[static_cast<size_t>(slot)].setBounds(tabRow.removeFromLeft(tabWidth));
            if (slot < (ModernAudioEngine::NumModSequencers - 1))
                tabRow.removeFromLeft(tabGap);
        }
        controlsArea.removeFromTop(compactGap);

        auto row0 = controlsArea.removeFromTop(compactRowHeight);
        auto cols0 = splitRow(row0, gap);
        auto row0Left = cols0.first;
        modTargetLabel.setBounds(row0Left.removeFromLeft(36));
        modTargetBox.setBounds(row0Left);
        modLengthLabel.setBounds(cols0.second.removeFromLeft(22));
        modLengthBox.setBounds(cols0.second.removeFromLeft(60));

        controlsArea.removeFromTop(compactGap);
        auto row1 = controlsArea.removeFromTop(compactRowHeight);
        auto cols1 = splitRow(row1, gap);
        auto row1Left = cols1.first;
        modDepthLabel.setBounds(row1Left.removeFromLeft(36));
        modDepthSlider.setBounds(row1Left);
        modBipolarToggle.setBounds(cols1.second);

        controlsArea.removeFromTop(compactGap);
        auto row2 = controlsArea.removeFromTop(compactRowHeight);
        auto cols2 = splitRow(row2, gap);
        auto row2Left = cols2.first;
        modOffsetLabel.setBounds(row2Left.removeFromLeft(36));
        modOffsetSlider.setBounds(row2Left);
        modCurveBendLabel.setBounds(cols2.second.removeFromLeft(30));
        modCurveBendSlider.setBounds(cols2.second);

        controlsArea.removeFromTop(compactGap);
        auto row3 = controlsArea.removeFromTop(compactRowHeight);
        auto cols3 = splitRow(row3, gap);
        modPitchQuantToggle.setBounds(cols3.first);
        modPitchScaleBox.setBounds(cols3.second);

        controlsArea.removeFromTop(compactGap);
        auto row4 = controlsArea.removeFromTop(compactRowHeight);
        auto cols4 = splitRow(row4, gap);
        modCurveTypeLabel.setBounds(cols4.first.removeFromLeft(30));
        modCurveTypeBox.setBounds(cols4.first);
        modShapeLabel.setBounds(cols4.second.removeFromLeft(30));
        modShapeBox.setBounds(cols4.second);
        return;
    }

    loadButton.setVisible(true);
    transientSliceButton.setVisible(true);
    playModeBox.setVisible(true);
    directionModeBox.setVisible(true);
    groupSelector.setVisible(true);
    modTargetLabel.setVisible(false);
    modTargetBox.setVisible(false);
    modBipolarToggle.setVisible(false);
    modDepthLabel.setVisible(false);
    modDepthSlider.setVisible(false);
    modOffsetLabel.setVisible(false);
    modOffsetSlider.setVisible(false);
    modCurveBendLabel.setVisible(false);
    modCurveBendSlider.setVisible(false);
    modLengthLabel.setVisible(false);
    modLengthBox.setVisible(false);
    for (auto& tab : modSequencerTabs)
        tab.setVisible(false);
    modPitchQuantToggle.setVisible(false);
    modPitchScaleBox.setVisible(false);
    modShapeLabel.setVisible(false);
    modShapeBox.setVisible(false);
    modCurveTypeLabel.setVisible(false);
    modCurveTypeBox.setVisible(false);
    
    // Controls column on right
    controlsArea.reduce(4, 0);
    
    const bool isGrainMode = grainOverlayVisible;
    const bool isGrainSpacePage = isGrainMode && grainSubPage == GrainSubPage::Space;
    const bool isStepMode = showingStepDisplay;
    patternLengthBox.setBounds({});
    stepLengthReadoutBox.setBounds({});

    const int rowGap = isGrainMode ? 0 : 1;

    // Top row: Load + slice mode
    const int topRowHeight = isGrainMode ? (isGrainSpacePage ? 12 : 14) : 18;
    auto topRow = controlsArea.removeFromTop(topRowHeight);
    const int half = topRow.getWidth() / 2;
    auto loadArea = topRow.removeFromLeft(half);
    loadButton.setBounds(loadArea.reduced(0, 0));
    topRow.removeFromLeft(2);
    transientSliceButton.setBounds(topRow);
    controlsArea.removeFromTop(rowGap);
    
    // Second row: Play mode and Direction mode (2/3 width each), Group (1/3 width)
    const int modesRowHeight = isGrainMode ? (isGrainSpacePage ? 12 : 14) : 18;
    auto modesRow = controlsArea.removeFromTop(modesRowHeight);
    int thirdWidth = modesRow.getWidth() / 3;
    playModeBox.setBounds(modesRow.removeFromLeft(thirdWidth).reduced(1, 0));
    directionModeBox.setBounds(modesRow.removeFromLeft(thirdWidth).reduced(1, 0));
    groupSelector.setBounds(modesRow.reduced(1, 0));  // Remaining space
    controlsArea.removeFromTop(rowGap);
    
    // Check if we have enough height for compact transport + record controls.
    const int requiredTopControlsHeight = 22 + 2 + 20 + 2 + 30 + 10 + 10;
    bool showTempoControls = (!isGrainMode) && (controlsArea.getHeight() >= requiredTopControlsHeight);

    // Update visibility
    const bool showRecordBars = (!isGrainMode) && controlsArea.getHeight() >= 18;
    tempoLabel.setVisible(showTempoControls);
    recordBarsBox.setVisible(showRecordBars);
    recordButton.setVisible(showRecordBars);
    recordBarsLabel.setVisible(false);

    // Tempo controls row - only if we have space
    if (showTempoControls)
    {
        auto tempoRow = controlsArea.removeFromTop(22);
        tempoLabel.setBounds(tempoRow.removeFromLeft(44));
        controlsArea.removeFromTop(2);

        auto recBarsRow = controlsArea.removeFromTop(18);
        recordBarsBox.setBounds(recBarsRow.removeFromLeft(70));
        recBarsRow.removeFromLeft(8);
        recordButton.setBounds(recBarsRow.removeFromLeft(46));
        if (isStepMode)
        {
            recBarsRow.removeFromLeft(4);
            const int lenWidth = juce::jlimit(18, 26, recBarsRow.getWidth());
            patternLengthBox.setBounds(recBarsRow.removeFromLeft(lenWidth));
            if (recBarsRow.getWidth() > 0)
            {
                recBarsRow.removeFromLeft(3);
                const int readoutWidth = juce::jlimit(34, 62, recBarsRow.getWidth());
                stepLengthReadoutBox.setBounds(recBarsRow.removeFromLeft(readoutWidth));
            }
        }
        controlsArea.removeFromTop(2);
    }
    else if (showRecordBars)
    {
        auto recBarsRow = controlsArea.removeFromTop(16);
        recordBarsBox.setBounds(recBarsRow.removeFromLeft(66));
        recBarsRow.removeFromLeft(8);
        recordButton.setBounds(recBarsRow.removeFromLeft(42));
        if (isStepMode)
        {
            recBarsRow.removeFromLeft(4);
            const int lenWidth = juce::jlimit(18, 24, recBarsRow.getWidth());
            patternLengthBox.setBounds(recBarsRow.removeFromLeft(lenWidth));
            if (recBarsRow.getWidth() > 0)
            {
                recBarsRow.removeFromLeft(3);
                const int readoutWidth = juce::jlimit(32, 58, recBarsRow.getWidth());
                stepLengthReadoutBox.setBounds(recBarsRow.removeFromLeft(readoutWidth));
            }
        }
        controlsArea.removeFromTop(2);
    }
    else if (isStepMode && controlsArea.getHeight() >= 14)
    {
        auto lenRow = controlsArea.removeFromTop(16);
        const int lenWidth = juce::jlimit(18, 24, lenRow.getWidth());
        patternLengthBox.setBounds(lenRow.removeFromLeft(lenWidth));
        if (lenRow.getWidth() > 0)
        {
            lenRow.removeFromLeft(3);
            const int readoutWidth = juce::jlimit(32, 58, lenRow.getWidth());
            stepLengthReadoutBox.setBounds(lenRow.removeFromLeft(readoutWidth));
        }
        controlsArea.removeFromTop(2);
    }
    
    // Rotary knobs row.
    const int knobsRowHeight = isGrainMode ? (isGrainSpacePage ? 20 : 22) : 30;
    auto knobsRow = controlsArea.removeFromTop(knobsRowHeight);
    int totalWidth = knobsRow.getWidth();
    int mainKnobsWidth = (totalWidth * 7) / 10;
    int mainKnobWidth = mainKnobsWidth / 3;

    if (isGrainMode)
    {
        grainSizeSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        grainDensitySlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        knobsRow.removeFromLeft(mainKnobWidth);
    }
    else if (isStepMode)
    {
        const int stepGap = 2;
        const int stepKnobWidth = juce::jmax(8, (knobsRow.getWidth() - (4 * stepGap)) / 5);
        volumeSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        panSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepAttackSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepDecaySlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepReleaseSlider.setBounds(knobsRow.reduced(1));
    }
    else
    {
        std::array<juce::Slider*, 6> visibleKnobs{};
        int visibleCount = 0;
        auto addVisibleKnob = [&](juce::Slider& knob)
        {
            if (knob.isVisible() && visibleCount < static_cast<int>(visibleKnobs.size()))
                visibleKnobs[static_cast<size_t>(visibleCount++)] = &knob;
        };
        addVisibleKnob(volumeSlider);
        addVisibleKnob(panSlider);
        addVisibleKnob(pitchSlider);
        addVisibleKnob(speedSlider);
        addVisibleKnob(scratchSlider);
        addVisibleKnob(sliceLengthSlider);

        const bool denseLayout = visibleCount >= 6;
        const int knobGap = denseLayout ? 1 : 2;
        const int totalGap = knobGap * juce::jmax(0, visibleCount - 1);
        const int knobWidth = juce::jmax(8, (knobsRow.getWidth() - totalGap) / juce::jmax(1, visibleCount));
        for (int i = 0; i < visibleCount; ++i)
        {
            if (auto* knob = visibleKnobs[static_cast<size_t>(i)])
                knob->setBounds(knobsRow.removeFromLeft(knobWidth).reduced(denseLayout ? 2 : 1));
            if (i < (visibleCount - 1))
                knobsRow.removeFromLeft(knobGap);
        }
    }

    const int labelsRowHeight = isGrainMode ? (isGrainSpacePage ? 9 : 10) : 9;
    auto labelsRow = controlsArea.removeFromTop(labelsRowHeight);
    if (isGrainMode)
    {
        auto sizeLabelArea = labelsRow.removeFromLeft(mainKnobWidth);
        const int syncToggleW = 14;
        const int syncModeW = 30;
        const int labelW = juce::jmax(0, sizeLabelArea.getWidth() - syncToggleW - syncModeW - 4);
        grainSizeLabel.setBounds(sizeLabelArea.removeFromLeft(labelW));
        sizeLabelArea.removeFromLeft(2);
        grainSizeSyncToggle.setBounds(sizeLabelArea.removeFromLeft(syncToggleW));
        sizeLabelArea.removeFromLeft(2);
        grainSizeDivLabel.setBounds(sizeLabelArea.removeFromLeft(syncModeW));
        grainDensityLabel.setBounds(labelsRow.removeFromLeft(mainKnobWidth));
        labelsRow.removeFromLeft(mainKnobWidth);
    }
    else if (isStepMode)
    {
        const int stepGap = 2;
        const int stepLabelWidth = juce::jmax(8, (labelsRow.getWidth() - (4 * stepGap)) / 5);
        volumeLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        panLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepAttackLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepDecayLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepReleaseLabel.setBounds(labelsRow);
    }
    else
    {
        std::array<juce::Label*, 6> visibleLabels{};
        int visibleCount = 0;
        auto addVisibleLabel = [&](juce::Label& label)
        {
            if (label.isVisible() && visibleCount < static_cast<int>(visibleLabels.size()))
                visibleLabels[static_cast<size_t>(visibleCount++)] = &label;
        };
        addVisibleLabel(volumeLabel);
        addVisibleLabel(panLabel);
        addVisibleLabel(pitchLabel);
        addVisibleLabel(speedLabel);
        addVisibleLabel(scratchLabel);
        addVisibleLabel(sliceLengthLabel);

        const int labelGap = 2;
        const int totalGap = labelGap * juce::jmax(0, visibleCount - 1);
        const int labelWidth = juce::jmax(8, (labelsRow.getWidth() - totalGap) / juce::jmax(1, visibleCount));
        for (int i = 0; i < visibleCount; ++i)
        {
            if (auto* label = visibleLabels[static_cast<size_t>(i)])
                label->setBounds(labelsRow.removeFromLeft(labelWidth));
            if (i < (visibleCount - 1))
                labelsRow.removeFromLeft(labelGap);
        }
    }
    if (!isGrainMode)
    {
        if (controlsArea.getHeight() >= 10)
        {
            // Recording loop length label (small, at bottom)
            recordLengthLabel.setBounds(controlsArea.removeFromTop(10));
        }
        return;
    }

    controlsArea.removeFromTop(isGrainSpacePage ? 0 : 1);
    auto tabRow = controlsArea.removeFromTop(isGrainSpacePage ? 11 : 13);
    const int tabGap = 2;
    const int tabW = juce::jmax(1, (tabRow.getWidth() - (2 * tabGap)) / 3);
    grainTabPitchButton.setBounds(tabRow.removeFromLeft(tabW));
    tabRow.removeFromLeft(tabGap);
    grainTabSpaceButton.setBounds(tabRow.removeFromLeft(tabW));
    tabRow.removeFromLeft(tabGap);
    grainTabShapeButton.setBounds(tabRow);

    controlsArea.removeFromTop(isGrainSpacePage ? 1 : 2);
    const int rowGapMini = isGrainSpacePage ? 1 : 2;
    const int totalMiniRows = (grainSubPage == GrainSubPage::Space) ? 3 : 2;
    const int minMiniRowH = isGrainSpacePage ? 8 : 12;
    const int maxMiniRowH = isGrainSpacePage ? 20 : 22;
    const int rowH = juce::jlimit(minMiniRowH,
                                  maxMiniRowH,
                                  (controlsArea.getHeight() - ((totalMiniRows - 1) * rowGapMini)) / totalMiniRows);
    const int miniLabelW = isGrainSpacePage ? 30 : 34;
    auto layoutGrainMiniRow = [&](juce::Label& labelA, juce::Slider& sliderA,
                                  juce::Label& labelB, juce::Slider& sliderB)
    {
        if (controlsArea.getHeight() <= 0)
            return;
        auto row = controlsArea.removeFromTop(rowH);
        auto left = row.removeFromLeft(row.getWidth() / 2);
        labelA.setBounds(left.removeFromLeft(miniLabelW));
        sliderA.setBounds(left);
        row.removeFromLeft(2);
        labelB.setBounds(row.removeFromLeft(miniLabelW));
        sliderB.setBounds(row);
        if (controlsArea.getHeight() > rowGapMini)
            controlsArea.removeFromTop(rowGapMini);
    };
    auto layoutGrainSingleRow = [&](juce::Label& label, juce::Slider& slider)
    {
        if (controlsArea.getHeight() <= 0)
            return;
        auto row = controlsArea.removeFromTop(rowH);
        label.setBounds(row.removeFromLeft(miniLabelW));
        slider.setBounds(row);
        if (controlsArea.getHeight() > rowGapMini)
            controlsArea.removeFromTop(rowGapMini);
    };

    if (grainSubPage == GrainSubPage::Pitch)
    {
        if (controlsArea.getHeight() > 0)
        {
            layoutGrainMiniRow(grainPitchLabel, grainPitchSlider, grainPitchJitterLabel, grainPitchJitterSlider);
        }
        layoutGrainMiniRow(grainArpLabel, grainArpSlider, grainRandomLabel, grainRandomSlider);
    }
    else if (grainSubPage == GrainSubPage::Space)
    {
        layoutGrainMiniRow(grainSpreadLabel, grainSpreadSlider, grainJitterLabel, grainJitterSlider);
        layoutGrainMiniRow(grainCloudLabel, grainCloudSlider, grainEmitterLabel, grainEmitterSlider);
        layoutGrainSingleRow(grainPositionJitterLabel, grainPositionJitterSlider);
    }
    else
    {
        layoutGrainMiniRow(grainEnvelopeLabel, grainEnvelopeSlider, grainShapeLabel, grainShapeSlider);
    }
}


void StripControl::loadSample()
{
    // Get current play mode to determine which path to use
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    bool isStepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    auto mode = isStepMode ? MlrVSTAudioProcessor::SamplePathMode::Step
                           : MlrVSTAudioProcessor::SamplePathMode::Loop;
    juce::File startingDirectory = processor.getDefaultSampleDirectory(stripIndex, mode);
    
    // If no last path, use default
    if (!startingDirectory.exists())
        startingDirectory = juce::File();
    
    juce::FileChooser chooser("Load Sample", startingDirectory, "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");
    
    if (chooser.browseForFileToOpen())
    {
        loadSampleFromFile(chooser.getResult());
    }
}

bool StripControl::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    return file.hasFileExtension(".wav;.aif;.aiff;.mp3;.ogg;.flac");
}

void StripControl::loadSampleFromFile(const juce::File& file)
{
    if (!isSupportedAudioFile(file))
        return;

    processor.loadSampleToStrip(stripIndex, file);

    auto* strip = processor.getAudioEngine() ? processor.getAudioEngine()->getStrip(stripIndex) : nullptr;
    const bool isStepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    auto mode = isStepMode ? MlrVSTAudioProcessor::SamplePathMode::Step
                           : MlrVSTAudioProcessor::SamplePathMode::Loop;
    processor.setDefaultSampleDirectory(stripIndex, mode, file.getParentDirectory());
}

bool StripControl::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        if (isSupportedAudioFile(juce::File(path)))
            return true;
    }
    return false;
}

void StripControl::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    for (const auto& path : files)
    {
        juce::File file(path);
        if (isSupportedAudioFile(file))
        {
            loadSampleFromFile(file);
            break;
        }
    }
}

void StripControl::updateFromEngine()
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip) return;
    const auto modState = processor.getAudioEngine()->getModSequencerState(stripIndex);
    const auto modulates = [&](ModernAudioEngine::ModTarget t)
    {
        return modState.target == t;
    };

    if (modulationLaneView)
    {
        const auto mod = processor.getAudioEngine()->getModSequencerState(stripIndex);
        modTargetBox.setSelectedId(modTargetToComboId(mod.target), juce::dontSendNotification);
        modBipolarToggle.setToggleState(mod.bipolar, juce::dontSendNotification);
        modBipolarToggle.setEnabled(modTargetAllowsBipolar(mod.target));
        modDepthSlider.setValue(mod.depth, juce::dontSendNotification);
        modOffsetSlider.setValue(mod.smoothingMs, juce::dontSendNotification);
        modCurveBendSlider.setValue(mod.curveBend, juce::dontSendNotification);
        modLengthBox.setSelectedId(mod.lengthBars, juce::dontSendNotification);
        modPitchQuantToggle.setToggleState(mod.pitchScaleQuantize, juce::dontSendNotification);
        modPitchScaleBox.setSelectedId(pitchScaleToComboId(static_cast<ModernAudioEngine::PitchScale>(mod.pitchScale)), juce::dontSendNotification);
        modPitchScaleBox.setEnabled(mod.pitchScaleQuantize);
        modShapeBox.setSelectedId(mod.curveMode ? 1 : 2, juce::dontSendNotification);
        modCurveTypeBox.setSelectedId(curveShapeToComboId(static_cast<ModernAudioEngine::ModCurveShape>(mod.curveShape)),
                                      juce::dontSendNotification);
        modCurveBendSlider.setEnabled(mod.curveMode);
        modCurveTypeBox.setEnabled(mod.curveMode);
        updateModSequencerTabButtons();
        repaint();
        return;
    }

    const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    if (showingStepDisplay != isStepMode)
    {
        showingStepDisplay = isStepMode;
        waveform.setVisible(!isStepMode);
        stepDisplay.setVisible(isStepMode);
        patternLengthBox.setVisible(isStepMode);
        stepLengthReadoutBox.setVisible(isStepMode);
        updateGrainOverlayVisibility();
        resized();
    }
    
    // Update step display if in step mode
    if (showingStepDisplay)
    {
        stepDisplay.setStepPattern(strip->stepPattern, strip->getStepTotalSteps());
        stepDisplay.setStepSubdivisions(strip->stepSubdivisions);
        stepDisplay.setStepSubdivisionVelocityRange(strip->stepSubdivisionStartVelocity,
                                                    strip->stepSubdivisionRepeatVelocity);
        stepDisplay.setStepProbability(strip->stepProbability);
        stepDisplay.setCurrentStep(strip->currentStep);
        stepDisplay.setPlaying(strip->isPlaying());

        if (processor.isStepEditModeActive())
        {
            StepSequencerDisplay::EditTool mappedTool = StepSequencerDisplay::EditTool::Volume;
            switch (processor.getStepEditToolIndex())
            {
                case 1: mappedTool = StepSequencerDisplay::EditTool::Volume; break;
                case 2: mappedTool = StepSequencerDisplay::EditTool::Divide; break;
                case 3: mappedTool = StepSequencerDisplay::EditTool::RampUp; break;
                case 4: mappedTool = StepSequencerDisplay::EditTool::RampDown; break;
                case 5: mappedTool = StepSequencerDisplay::EditTool::Probability; break;
                case 0:
                case 6:
                case 7:
                case 8:
                default: mappedTool = StepSequencerDisplay::EditTool::Volume; break;
            }

            if (stepDisplay.getActiveTool() != mappedTool)
                stepDisplay.setActiveTool(mappedTool);
        }

        // No playback position indicator in step mode - just show steps
    }
    
    // Update waveform display (only if visible - i.e., not in step mode)
    if (!showingStepDisplay && strip->hasAudio())
    {
        auto* buffer = strip->getAudioBuffer();
        if (buffer && buffer->getNumSamples() > 0)
        {
            waveform.setAudioBuffer(*buffer, strip->getSourceSampleRate());
            waveform.setLoopPoints(strip->getLoopStart(), strip->getLoopEnd(), 16);
            waveform.setSliceMarkers(strip->getSliceStartSamples(false),
                                     strip->getSliceStartSamples(true),
                                     buffer->getNumSamples(),
                                     strip->isTransientSliceMode());
            
            if (strip->isPlaying() || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            {
                double playbackPos = strip->getPlaybackPosition();
                double numSamples = static_cast<double>(buffer->getNumSamples());
                
                // Safety check to prevent division by zero or NaN
                if (numSamples > 0 && std::isfinite(playbackPos))
                {
                    double wrappedPos = std::fmod(playbackPos, numSamples);
                    if (wrappedPos < 0.0)
                        wrappedPos += numSamples;
                    double normalized = wrappedPos / numSamples;
                    waveform.setPlaybackPosition(normalized);
                }
            }

            const bool isGrainMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
            double grainWindowNorm = 0.0;
            if (isGrainMode && buffer->getNumSamples() > 0 && strip->getSourceSampleRate() > 0.0)
            {
                double sizeMsForDisplay = static_cast<double>(strip->getGrainSizeMs());
                const double hostTempo = juce::jmax(1.0, processor.getAudioEngine()->getCurrentTempo());
                static constexpr std::array<double, 13> sizeDivisionsBeats {
                    1.0 / 64.0, 1.0 / 48.0, 1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0,
                    1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0,
                    1.0 / 2.0, 1.0, 2.0
                };
                const double t = juce::jlimit(0.0, 1.0, (sizeMsForDisplay - 5.0) / (2400.0 - 5.0));
                const int idx = juce::jlimit(0, static_cast<int>(sizeDivisionsBeats.size()) - 1,
                                             static_cast<int>(std::round(t * static_cast<double>(sizeDivisionsBeats.size() - 1))));
                if (strip->isGrainTempoSyncEnabled())
                    sizeMsForDisplay = sizeDivisionsBeats[static_cast<size_t>(idx)] * (60.0 / hostTempo) * 1000.0;
                const double sizeSamples = (sizeMsForDisplay * 0.001) * strip->getSourceSampleRate();
                grainWindowNorm = sizeSamples / static_cast<double>(buffer->getNumSamples());
            }
            waveform.setGrainWindowOverlay(isGrainMode, grainWindowNorm);
            waveform.setGrainMarkerPositions(strip->getGrainPreviewPositions(),
                                             strip->getGrainPreviewPitchNorms());
            waveform.setGrainHudOverlay(false, {}, {}, 0.0f, 0.0f, 0.0f,
                                        strip->getGrainPitch(), strip->getGrainArpDepth(), strip->getGrainPitchJitter());
        }
    }
    else if (!showingStepDisplay)
    {
        if (waveform.hasLoadedAudio())
            waveform.clear();
    }
    
    // Update tempo label - only if visible
    if (tempoLabel.isVisible())
    {
        float beats = strip->getBeatsPerLoop();
        
        // Simple, safe validation
        if (beats >= 0.25f && beats <= 64.0f && std::isfinite(beats))
        {
            // Valid range - format it
            tempoLabel.setText(juce::String(beats, 1) + "b", juce::dontSendNotification);
        }
        else
        {
            // Invalid or auto - show AUTO
            tempoLabel.setText("AUTO", juce::dontSendNotification);
        }
    }
    
    if (!speedSlider.isMouseButtonDown() && !modulates(ModernAudioEngine::ModTarget::Speed))
        speedSlider.setValue(getPlayheadSpeedRatioForStrip(*strip), juce::dontSendNotification);
    if (!scratchSlider.isMouseButtonDown())
        scratchSlider.setValue(strip->getScratchAmount(), juce::dontSendNotification);
    if (!sliceLengthSlider.isMouseButtonDown())
        sliceLengthSlider.setValue(strip->getLoopSliceLength(), juce::dontSendNotification);
    const int stepLength = strip->getStepPatternLengthSteps();
    if ((stepLength % 16) == 0)
        patternLengthBox.setSelectedId(juce::jlimit(1, 4, stepLength / 16), juce::dontSendNotification);
    else
    {
        patternLengthBox.setSelectedId(0, juce::dontSendNotification);
        patternLengthBox.setText(juce::String(stepLength), juce::dontSendNotification);
    }
    if (!stepLengthReadoutBox.isInteracting())
        stepLengthReadoutBox.setValue(stepLength, juce::dontSendNotification);
    if (!stepAttackSlider.isMouseButtonDown())
        stepAttackSlider.setValue(strip->getStepEnvelopeAttackMs(), juce::dontSendNotification);
    if (!stepDecaySlider.isMouseButtonDown())
        stepDecaySlider.setValue(strip->getStepEnvelopeDecayMs(), juce::dontSendNotification);
    if (!stepReleaseSlider.isMouseButtonDown())
        stepReleaseSlider.setValue(strip->getStepEnvelopeReleaseMs(), juce::dontSendNotification);
    {
        const float recordingBarsBeats = static_cast<float>(juce::jlimit(1, 8, strip->getRecordingBars()) * 4);
        float beats = strip->getBeatsPerLoop();
        if (!(beats > 0.0f && std::isfinite(beats)))
            beats = recordingBarsBeats;
        else if (strip->isPlaying() && beats >= 4.0f && std::abs(beats - recordingBarsBeats) > 0.01f)
            beats = recordingBarsBeats;

        struct BeatChoice { float beats; int id; };
        static constexpr BeatChoice choices[] {
            { 1.0f, 25 }, { 2.0f, 50 }, { 4.0f, 100 },
            { 8.0f, 200 }, { 16.0f, 400 }, { 32.0f, 800 }
        };

        int selectedId = 100;
        float best = std::numeric_limits<float>::max();
        for (const auto& c : choices)
        {
            const float d = std::abs(beats - c.beats);
            if (d < best)
            {
                best = d;
                selectedId = c.id;
            }
        }
        recordBarsBox.setSelectedId(selectedId, juce::dontSendNotification);
        recordBarsBox.setEnabled(processor.canChangeBarLengthNow(stripIndex));
    }
    const bool recordArmed = !strip->hasAudio();
    const bool blinkOn = processor.getAudioEngine()->shouldBlinkRecordLED();
    recordButton.setButtonText(recordArmed ? "ARM" : "REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           recordArmed
                               ? (blinkOn ? juce::Colour(0xffc95252) : juce::Colour(0xff743636))
                               : (blinkOn ? juce::Colour(0xffa64a4a) : juce::Colour(0xff444444)));
    recordButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff0f0f0));
    
    // Sync volume and pan from engine
    if (!modulates(ModernAudioEngine::ModTarget::Volume))
        volumeSlider.setValue(strip->getVolume(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::Pan))
        panSlider.setValue(strip->getPan(), juce::dontSendNotification);
    if (!pitchSlider.isMouseButtonDown() && !modulates(ModernAudioEngine::ModTarget::Pitch))
        pitchSlider.setValue(processor.getPitchSemitonesForDisplay(*strip), juce::dontSendNotification);
    // Sync play mode dropdown with strip state
    int modeId = static_cast<int>(strip->getPlayMode()) + 1;
    if (playModeBox.getSelectedId() != modeId)
        playModeBox.setSelectedId(modeId, juce::dontSendNotification);
    
    // Sync direction mode dropdown with strip state
    int dirModeId = static_cast<int>(strip->getDirectionMode()) + 1;
    if (directionModeBox.getSelectedId() != dirModeId)
        directionModeBox.setSelectedId(dirModeId, juce::dontSendNotification);

    const bool transientMode = strip->isTransientSliceMode();
    transientSliceButton.setToggleState(transientMode, juce::dontSendNotification);
    transientSliceButton.setButtonText(transientMode ? "TRANS" : "TIME");
    updateGrainOverlayVisibility();
    if (!modulates(ModernAudioEngine::ModTarget::GrainSize))
        grainSizeSlider.setValue(strip->getGrainSizeMs(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainDensity))
        grainDensitySlider.setValue(strip->getGrainDensity(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainPitch))
        grainPitchSlider.setValue(strip->getGrainPitch(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainPitchJitter))
        grainPitchJitterSlider.setValue(strip->getGrainPitchJitter(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainSpread))
        grainSpreadSlider.setValue(strip->getGrainSpread(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainJitter))
        grainJitterSlider.setValue(strip->getGrainJitter(), juce::dontSendNotification);
    if (!grainPositionJitterSlider.isMouseButtonDown())
        grainPositionJitterSlider.setValue(strip->getGrainPositionJitter(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainRandom))
        grainRandomSlider.setValue(strip->getGrainRandomDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainArp))
        grainArpSlider.setValue(strip->getGrainArpDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainCloud))
        grainCloudSlider.setValue(strip->getGrainCloudDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainEmitter))
        grainEmitterSlider.setValue(strip->getGrainEmitterDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainEnvelope))
        grainEnvelopeSlider.setValue(strip->getGrainEnvelope(), juce::dontSendNotification);
    grainShapeSlider.setValue(strip->getGrainShape(), juce::dontSendNotification);
    const bool grainSyncEnabled = strip->isGrainTempoSyncEnabled();
    grainSizeSyncToggle.setToggleState(grainSyncEnabled, juce::dontSendNotification);
    grainSizeDivLabel.setText(grainSyncEnabled ? "SYNC" : "FREE", juce::dontSendNotification);
    grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    {
        const bool arpActive = strip->getGrainArpDepth() > 0.001f;
        grainPitchLabel.setText(arpActive ? "RANGE" : "PITCH", juce::dontSendNotification);
        if (!grainPitchSlider.isMouseButtonDown())
        {
            if (arpActive)
            {
                grainPitchSlider.setRange(0.0, 48.0, 0.1);
                grainPitchSlider.setValue(std::abs(strip->getGrainPitch()), juce::dontSendNotification);
            }
            else
            {
                grainPitchSlider.setRange(-48.0, 48.0, 0.1);
                grainPitchSlider.setValue(strip->getGrainPitch(), juce::dontSendNotification);
            }
        }
    }

    // Sync group selector from engine
    int currentGroup = strip->getGroup();
    int selectedId = currentGroup + 2;  // Convert: -1→1, 0→2, 1→3, 2→4, 3→5
    if (groupSelector.getSelectedId() != selectedId)
    {
        groupSelector.setSelectedId(selectedId, juce::dontSendNotification);
    }

    // Mod target pulse indication on actual control colours (not label text).
    auto tintSlider = [](juce::Slider& s, juce::Colour c, float pulseAmount)
    {
        const float pulse = juce::jlimit(0.0f, 1.0f, pulseAmount);
        const auto fill = c.interpolatedWith(kAccent.brighter(0.5f), 0.25f * pulse);
        s.setColour(juce::Slider::rotarySliderFillColourId, fill);
        s.setColour(juce::Slider::trackColourId, fill.withAlpha(0.78f + (0.2f * pulse)));
        s.setColour(juce::Slider::thumbColourId, fill.brighter(0.18f + (0.42f * pulse)));
        s.setColour(juce::Slider::rotarySliderOutlineColourId,
                    juce::Colour(0xff4a4a4a).interpolatedWith(fill.brighter(0.55f), 0.7f * pulse));
    };
    auto setModIndicator = [](juce::Slider& s, bool active, float depth, float signedPos, juce::Colour colour)
    {
        auto& props = s.getProperties();
        props.set("modActive", active);
        props.set("modDepth", juce::jlimit(0.0f, 1.0f, depth));
        props.set("modSigned", juce::jlimit(-1.0f, 1.0f, signedPos));
        props.set("modColour", static_cast<int>(colour.getARGB()));
    };
    auto pickVisibleModColour = [](const juce::Slider& s)
    {
        const auto base = s.findColour(juce::Slider::rotarySliderFillColourId);
        const float hue = base.getHue();
        const bool nearYellowHue = (hue > 0.10f && hue < 0.18f) && base.getSaturation() > 0.25f;
        const auto ref = juce::Colour(0xffffd24a);
        const float dr = base.getFloatRed() - ref.getFloatRed();
        const float dg = base.getFloatGreen() - ref.getFloatGreen();
        const float db = base.getFloatBlue() - ref.getFloatBlue();
        const float rgbDist = std::sqrt((dr * dr) + (dg * dg) + (db * db));
        const bool nearAccent = base.getPerceivedBrightness() > 0.45f
                             && rgbDist < 0.34f;
        if (nearYellowHue || nearAccent)
            return juce::Colour(0xff3bd5ff); // cyan contrast for yellow/orange controls
        return juce::Colour(0xffffd24a);     // default warm modulation color
    };
    const auto baseControl = stripColor.withAlpha(0.72f);
    tintSlider(volumeSlider, baseControl, 0.0f);
    tintSlider(panSlider, baseControl, 0.0f);
    tintSlider(pitchSlider, baseControl, 0.0f);
    tintSlider(speedSlider, baseControl, 0.0f);
    tintSlider(scratchSlider, baseControl, 0.0f);
    tintSlider(sliceLengthSlider, baseControl, 0.0f);
    tintSlider(grainSizeSlider, baseControl, 0.0f);
    tintSlider(grainDensitySlider, baseControl, 0.0f);
    tintSlider(grainPitchSlider, baseControl, 0.0f);
    tintSlider(grainPitchJitterSlider, baseControl, 0.0f);
    tintSlider(grainSpreadSlider, baseControl, 0.0f);
    tintSlider(grainJitterSlider, baseControl, 0.0f);
    tintSlider(grainPositionJitterSlider, baseControl, 0.0f);
    tintSlider(grainRandomSlider, baseControl, 0.0f);
    tintSlider(grainArpSlider, baseControl, 0.0f);
    tintSlider(grainCloudSlider, baseControl, 0.0f);
    tintSlider(grainEmitterSlider, baseControl, 0.0f);
    tintSlider(grainEnvelopeSlider, baseControl, 0.0f);
    tintSlider(grainShapeSlider, baseControl, 0.0f);
    setModIndicator(volumeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(panSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(pitchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(speedSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(scratchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(sliceLengthSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSizeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainDensitySlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSpreadSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPositionJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainRandomSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainArpSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainCloudSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEmitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEnvelopeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainShapeSlider, false, 0.0f, 0.0f, kAccent);

    if (auto* engine = processor.getAudioEngine())
    {
        const auto mod = engine->getModSequencerState(stripIndex);
        if (mod.target != ModernAudioEngine::ModTarget::None)
        {
            const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
            const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
            const int activeStep = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
            const float raw = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, activeStep));
            const bool bipolar = mod.bipolar && modTargetAllowsBipolar(mod.target);
            const float depth = juce::jlimit(0.0f, 1.0f, mod.depth);
            const float modNorm = juce::jlimit(0.0f, 1.0f, raw * depth);
            const float modBi = juce::jlimit(-1.0f, 1.0f, ((raw * 2.0f) - 1.0f) * depth);
            const float intensity = bipolar ? std::abs(modBi) : modNorm;
            const float signedPos = juce::jlimit(-1.0f, 1.0f, (raw * 2.0f) - 1.0f);

            const float stepPulse = ((activeStep & 1) == 0) ? 1.0f : 0.65f;
            const float pulseAmount = juce::jlimit(0.0f, 1.0f,
                                                   (0.35f + (0.65f * juce::jmax(0.2f, intensity))) * stepPulse);

            auto* targetSlider = [&]() -> juce::Slider*
            {
                switch (mod.target)
                {
                    case ModernAudioEngine::ModTarget::None: return nullptr;
                    case ModernAudioEngine::ModTarget::Volume: return &volumeSlider;
                    case ModernAudioEngine::ModTarget::Pan: return &panSlider;
                    case ModernAudioEngine::ModTarget::Pitch: return &pitchSlider;
                    case ModernAudioEngine::ModTarget::Speed: return &speedSlider;
                    case ModernAudioEngine::ModTarget::Cutoff: return nullptr;
                    case ModernAudioEngine::ModTarget::Resonance: return nullptr;
                    case ModernAudioEngine::ModTarget::GrainSize: return &grainSizeSlider;
                    case ModernAudioEngine::ModTarget::GrainDensity: return &grainDensitySlider;
                    case ModernAudioEngine::ModTarget::GrainPitch: return &grainPitchSlider;
                    case ModernAudioEngine::ModTarget::GrainPitchJitter: return &grainPitchJitterSlider;
                    case ModernAudioEngine::ModTarget::GrainSpread: return &grainSpreadSlider;
                    case ModernAudioEngine::ModTarget::GrainJitter: return &grainJitterSlider;
                    case ModernAudioEngine::ModTarget::GrainRandom: return &grainRandomSlider;
                    case ModernAudioEngine::ModTarget::GrainArp: return &grainArpSlider;
                    case ModernAudioEngine::ModTarget::GrainCloud: return &grainCloudSlider;
                    case ModernAudioEngine::ModTarget::GrainEmitter: return &grainEmitterSlider;
                    case ModernAudioEngine::ModTarget::GrainEnvelope: return &grainEnvelopeSlider;
                    case ModernAudioEngine::ModTarget::Retrigger: return nullptr;
                    default: return nullptr;
                }
            }();
            if (targetSlider != nullptr)
            {
                const auto targetColour = pickVisibleModColour(*targetSlider);
                const auto pulseColour = targetColour.withAlpha(0.82f + (0.18f * pulseAmount));
                tintSlider(*targetSlider, pulseColour, pulseAmount);
                setModIndicator(*targetSlider, true, depth, signedPos, targetColour);
            }
        }
    }
    
    repaint();  // For LED overlay
}

//==============================================================================
// FXStripControl Implementation
//==============================================================================

FXStripControl::FXStripControl(int idx, MlrVSTAudioProcessor& p)
    : stripIndex(idx), processor(p)
{
    // Get strip color
    stripColor = getStripColor(idx);
    
    // Strip label exists but not visible (used internally if needed)
    stripLabel.setText("Strip " + juce::String(idx + 1), juce::dontSendNotification);
    stripLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    stripLabel.setColour(juce::Label::textColourId, stripColor);
    // DON'T add to view - no label shown
    
    // Filter Enable (button only, no text label)
    filterEnableButton.setButtonText("Filter");
    filterEnableButton.setClickingTogglesState(true);
    filterEnableButton.onClick = [this]() {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            const bool enabled = filterEnableButton.getToggleState();
            strip->setFilterEnabled(enabled);
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterEnabled(enabled);
            }
        }
    };
    addAndMakeVisible(filterEnableButton);
    
    // Filter Frequency
    filterFreqLabel.setText("Freq", juce::dontSendNotification);
    filterFreqLabel.setJustificationType(juce::Justification::centred);
    filterFreqLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterFreqLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterFreqLabel);
    
    filterFreqSlider.setSliderStyle(juce::Slider::Rotary);
    filterFreqSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 38, 12);
    filterFreqSlider.setRange(20.0, 20000.0, 1.0);
    filterFreqSlider.setSkewFactorFromMidPoint(1000.0);
    filterFreqSlider.setValue(20000.0);  // Default fully open (20kHz)
    enableAltClickReset(filterFreqSlider, 20000.0);
    filterFreqSlider.setTextValueSuffix(" Hz");
    filterFreqSlider.onValueChange = [this]() {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setFilterFrequency(static_cast<float>(filterFreqSlider.getValue()));
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterFrequency(static_cast<float>(filterFreqSlider.getValue()));
            }
        }
    };
    addAndMakeVisible(filterFreqSlider);
    
    // Filter Resonance
    filterResLabel.setText("Res", juce::dontSendNotification);
    filterResLabel.setJustificationType(juce::Justification::centred);
    filterResLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterResLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterResLabel);
    
    filterResSlider.setSliderStyle(juce::Slider::Rotary);
    filterResSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 35, 12);
    filterResSlider.setRange(0.1, 10.0, 0.01);
    filterResSlider.setValue(0.707);
    enableAltClickReset(filterResSlider, 0.707);
    filterResSlider.setTextValueSuffix(" Q");
    filterResSlider.onValueChange = [this]() {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setFilterResonance(static_cast<float>(filterResSlider.getValue()));
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterResonance(static_cast<float>(filterResSlider.getValue()));
            }
        }
    };
    addAndMakeVisible(filterResSlider);
    
    // Filter Morph
    filterMorphLabel.setText("Morph", juce::dontSendNotification);
    filterMorphLabel.setJustificationType(juce::Justification::centred);
    filterMorphLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterMorphLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterMorphLabel);

    filterMorphSlider.setSliderStyle(juce::Slider::Rotary);
    filterMorphSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 38, 12);
    filterMorphSlider.setRange(0.0, 1.0, 0.001);
    filterMorphSlider.setValue(0.0);
    filterMorphSlider.setDoubleClickReturnValue(true, 0.0);
    filterMorphSlider.textFromValueFunction = [](double value)
    {
        const double v = juce::jlimit(0.0, 1.0, value);
        if (v < 0.25) return juce::String("LP");
        if (v < 0.75) return juce::String("BP");
        return juce::String("HP");
    };
    filterMorphSlider.valueFromTextFunction = [](const juce::String& text)
    {
        const auto t = text.trim().toUpperCase();
        if (t.contains("LP")) return 0.0;
        if (t.contains("BP")) return 0.5;
        if (t.contains("HP")) return 1.0;
        return 0.0;
    };
    filterMorphSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            const float morphValue = static_cast<float>(filterMorphSlider.getValue());
            strip->setFilterMorph(morphValue);
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                EnhancedAudioStrip::FilterType stripType = EnhancedAudioStrip::FilterType::LowPass;
                FilterType stepType = FilterType::LowPass;
                if (morphValue >= 0.75f)
                {
                    stripType = EnhancedAudioStrip::FilterType::HighPass;
                    stepType = FilterType::HighPass;
                }
                else if (morphValue >= 0.25f)
                {
                    stripType = EnhancedAudioStrip::FilterType::BandPass;
                    stepType = FilterType::BandPass;
                }

                strip->setFilterType(stripType);
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterType(stepType);
            }
        }
    };
    addAndMakeVisible(filterMorphSlider);

    // Filter Algorithm selector
    filterAlgoLabel.setText("Alg", juce::dontSendNotification);
    filterAlgoLabel.setJustificationType(juce::Justification::centred);
    filterAlgoLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterAlgoLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterAlgoLabel);

    filterAlgoBox.addItem("SVF12", 1);
    filterAlgoBox.addItem("SVF24", 2);
    filterAlgoBox.addItem("LAD12", 3);
    filterAlgoBox.addItem("LAD24", 4);
    filterAlgoBox.addItem("MOOG S", 5);
#if MLRVST_ENABLE_HUOVILAINEN
    filterAlgoBox.addItem("MOOG H", 6);
#else
    filterAlgoBox.addItem("MOOG H*", 6);
#endif
    filterAlgoBox.setSelectedId(1);
    styleUiCombo(filterAlgoBox);
    filterAlgoBox.setJustificationType(juce::Justification::centred);
#if MLRVST_ENABLE_HUOVILAINEN
    filterAlgoBox.setTooltip("Filter algorithm: SVF12, SVF24, Ladder12, Ladder24, Moog Stilson LP, Moog Huovilainen LP");
#else
    filterAlgoBox.setTooltip("Filter algorithm: SVF12, SVF24, Ladder12, Ladder24, Moog Stilson LP, Moog H* (Stilson fallback; Huovilainen disabled in this build)");
#endif
    filterAlgoBox.onChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            const int id = filterAlgoBox.getSelectedId();
            auto algo = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
            if (id == 2) algo = EnhancedAudioStrip::FilterAlgorithm::Tpt24;
            else if (id == 3) algo = EnhancedAudioStrip::FilterAlgorithm::Ladder12;
            else if (id == 4) algo = EnhancedAudioStrip::FilterAlgorithm::Ladder24;
            else if (id == 5) algo = EnhancedAudioStrip::FilterAlgorithm::MoogStilson;
            else if (id == 6) algo = EnhancedAudioStrip::FilterAlgorithm::MoogHuov;
            strip->setFilterAlgorithm(algo);
        }
    };
    addAndMakeVisible(filterAlgoBox);

    // Start timer for updating from engine
    startTimer(50);  // Update at 20Hz
}

void FXStripControl::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    drawPanel(g, bounds, stripColor, 10.0f);

    // TWO vertical dividers creating 3 equal fields
    float thirdWidth = bounds.getWidth() / 3.0f;
    g.setColour(kPanelStroke.withAlpha(0.7f));

    // First divider (1/3 from left)
    float divider1X = bounds.getX() + thirdWidth;
    g.fillRect(divider1X - 1.0f, bounds.getY() + 20.0f, 2.0f, bounds.getHeight() - 40.0f);

    // Second divider (2/3 from left)
    float divider2X = bounds.getX() + (thirdWidth * 2.0f);
    g.fillRect(divider2X - 1.0f, bounds.getY() + 20.0f, 2.0f, bounds.getHeight() - 40.0f);
}

void FXStripControl::resized()
{
    auto bounds = getLocalBounds();
    
    // Match Play strip padding
    bounds.reduce(8, 8);
    
    // SPLIT INTO THREE FIELDS
    int fieldWidth = bounds.getWidth() / 3;

    // Field 1: Filter controls (left third)
    auto field1 = bounds.removeFromLeft(fieldWidth).reduced(6, 0);

    // Field 2/3: reserved for future controls
    auto field2 = bounds.removeFromLeft(fieldWidth).reduced(6, 0);
    auto field3 = bounds.reduced(6, 0);
    
    // Top row: Filter enable + compact algorithm selector
    auto topRow = field1.removeFromTop(22);
    filterEnableButton.setBounds(topRow.removeFromLeft(56));
    topRow.removeFromLeft(4);
    filterAlgoLabel.setBounds(topRow.removeFromLeft(24));
    topRow.removeFromLeft(3);
    filterAlgoBox.setBounds(topRow.removeFromLeft(92));
    field1.removeFromTop(4);
    
    // Three rotary controls: Freq | Res | Morph
    auto controlsRow = field1.removeFromTop(64);

    int controlWidth = controlsRow.getWidth() / 3;
    auto freqCol = controlsRow.removeFromLeft(controlWidth).reduced(2, 0);
    filterFreqLabel.setBounds(freqCol.removeFromTop(12));
    filterFreqSlider.setBounds(freqCol);

    auto resCol = controlsRow.removeFromLeft(controlWidth).reduced(2, 0);
    filterResLabel.setBounds(resCol.removeFromTop(12));
    filterResSlider.setBounds(resCol);

    auto morphCol = controlsRow.reduced(2, 0);
    filterMorphLabel.setBounds(morphCol.removeFromTop(12));
    filterMorphSlider.setBounds(morphCol);

    (void) field2;
    (void) field3;
}

void FXStripControl::updateFromEngine()
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip) return;

    const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? strip->getStepSampler() : nullptr;
    
    // Update from engine state
    const bool filterEnabled = (isStepMode && stepSampler) ? stepSampler->isFilterEnabled() : strip->isFilterEnabled();
    const float filterFreq = (isStepMode && stepSampler) ? stepSampler->getFilterFrequency() : strip->getFilterFrequency();
    const float filterRes = (isStepMode && stepSampler) ? stepSampler->getFilterResonance() : strip->getFilterResonance();
    float filterMorph = strip->getFilterMorph();
    if (isStepMode && stepSampler)
    {
        switch (stepSampler->getFilterType())
        {
            case FilterType::LowPass:  filterMorph = 0.0f; break;
            case FilterType::BandPass: filterMorph = 0.5f; break;
            case FilterType::HighPass: filterMorph = 1.0f; break;
            default: break;
        }
    }

    filterEnableButton.setToggleState(filterEnabled, juce::dontSendNotification);
    filterFreqSlider.setValue(filterFreq, juce::dontSendNotification);
    filterResSlider.setValue(filterRes, juce::dontSendNotification);
    filterMorphSlider.setValue(filterMorph, juce::dontSendNotification);

    const auto base = stripColor.withAlpha(0.72f);
    auto setBaseSliderTint = [](juce::Slider& s, juce::Colour c)
    {
        s.setColour(juce::Slider::rotarySliderFillColourId, c);
        s.setColour(juce::Slider::trackColourId, c.withAlpha(0.78f));
        s.setColour(juce::Slider::thumbColourId, c.brighter(0.18f));
        s.setColour(juce::Slider::rotarySliderOutlineColourId, c.darker(0.72f).withAlpha(0.82f));
    };
    auto pickVisibleModColour = [](juce::Colour baseColour)
    {
        const auto baseRgb = juce::Colour::fromRGB(baseColour.getRed(), baseColour.getGreen(), baseColour.getBlue());
        const auto accentRgb = juce::Colour::fromRGB(0xff, 0xd2, 0x4a);
        const auto dR = static_cast<float>(baseRgb.getFloatRed() - accentRgb.getFloatRed());
        const auto dG = static_cast<float>(baseRgb.getFloatGreen() - accentRgb.getFloatGreen());
        const auto dB = static_cast<float>(baseRgb.getFloatBlue() - accentRgb.getFloatBlue());
        const float rgbDist = std::sqrt((dR * dR) + (dG * dG) + (dB * dB));
        const bool nearYellowHue = baseColour.getHue() > 0.10f && baseColour.getHue() < 0.18f;
        const bool nearAccent = baseColour.getPerceivedBrightness() > 0.45f && rgbDist < 0.34f;
        return (nearYellowHue || nearAccent) ? juce::Colour(0xff3bd5ff) : juce::Colour(0xffffd24a);
    };

    setBaseSliderTint(filterFreqSlider, base);
    setBaseSliderTint(filterResSlider, base);

    const auto algo = strip->getFilterAlgorithm();
    int algoId = 1;
    if (algo == EnhancedAudioStrip::FilterAlgorithm::Tpt24) algoId = 2;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder12) algoId = 3;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder24) algoId = 4;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogStilson) algoId = 5;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogHuov) algoId = 6;
    filterAlgoBox.setSelectedId(algoId, juce::dontSendNotification);

    if (auto* engine = processor.getAudioEngine())
    {
        const auto mod = engine->getModSequencerState(stripIndex);
        const bool active = (mod.target == ModernAudioEngine::ModTarget::Cutoff
                          || mod.target == ModernAudioEngine::ModTarget::Resonance);
        if (active)
        {
            const float depth = juce::jlimit(0.0f, 1.0f, mod.depth);
            const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
            const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
            const int step = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
            const float raw = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, step));
            const bool bipolar = mod.bipolar && modTargetAllowsBipolar(mod.target);
            const float modNorm = juce::jlimit(0.0f, 1.0f, raw * depth);
            const float modBi = juce::jlimit(-1.0f, 1.0f, ((raw * 2.0f) - 1.0f) * depth);
            const float intensity = bipolar ? std::abs(modBi) : modNorm;
            const float stepPulse = ((step & 1) == 0) ? 1.0f : 0.65f;
            const float pulse = juce::jlimit(0.0f, 1.0f,
                                             (0.35f + (0.65f * juce::jmax(0.2f, intensity))) * stepPulse);
            const auto modColour = pickVisibleModColour(base).withAlpha(0.82f + (0.18f * pulse));
            if (mod.target == ModernAudioEngine::ModTarget::Cutoff)
                setBaseSliderTint(filterFreqSlider, modColour);
            else
                setBaseSliderTint(filterResSlider, modColour);
        }
    }
}

void FXStripControl::timerCallback()
{
    updateFromEngine();
}

void StripControl::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateFromEngine();
}

MonomeGridDisplay::MonomeGridDisplay(MlrVSTAudioProcessor& p)
    : processor(p)
{
    startTimer(50); // 20fps updates
}

void MonomeGridDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    
    // Background
    g.setColour(kSurfaceDark);
    g.fillRoundedRectangle(bounds.toFloat(), 8.0f);
    
    // Title
    g.setColour(kTextPrimary);
    g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    auto titleArea = bounds.removeFromTop(30);
    g.drawText("Monome Grid", titleArea, juce::Justification::centred);
    
    bounds.removeFromTop(4);
    
    // Draw grid
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            auto buttonBounds = getButtonBounds(x, y);
            
            // Button background
            g.setColour(juce::Colour(0xff2a2a2a));
            g.fillRoundedRectangle(buttonBounds.toFloat(), 2.0f);
            
            // LED state
            int brightness = ledState[x][y];
            if (brightness > 0)
            {
                float alpha = brightness / 15.0f;
                g.setColour(kAccent.withAlpha(alpha));
                g.fillRoundedRectangle(buttonBounds.toFloat().reduced(2), 2.0f);
            }
            
            // Pressed state
            if (buttonPressed[x][y])
            {
                g.setColour(kTextPrimary.withAlpha(0.25f));
                g.fillRoundedRectangle(buttonBounds.toFloat(), 2.0f);
            }
            
            // Border
            g.setColour(kPanelStroke);
            g.drawRoundedRectangle(buttonBounds.toFloat(), 2.0f, 1.0f);
        }
    }
}

void MonomeGridDisplay::resized()
{
    repaint();
}

juce::Rectangle<int> MonomeGridDisplay::getButtonBounds(int x, int y) const
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(34); // Title area
    
    auto buttonSize = juce::jmin(
        bounds.getWidth() / gridWidth - 4,
        bounds.getHeight() / gridHeight - 4
    );
    
    auto gridStartX = (bounds.getWidth() - (buttonSize + 4) * gridWidth) / 2;
    auto gridStartY = bounds.getY() + (bounds.getHeight() - (buttonSize + 4) * gridHeight) / 2;
    
    return juce::Rectangle<int>(
        gridStartX + x * (buttonSize + 4),
        gridStartY + y * (buttonSize + 4),
        buttonSize,
        buttonSize
    );
}

void MonomeGridDisplay::mouseDown(const juce::MouseEvent& e)
{
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (getButtonBounds(x, y).contains(e.getPosition()))
            {
                handleButtonPress(x, y, true);
                return;
            }
        }
    }
}

void MonomeGridDisplay::mouseUp(const juce::MouseEvent& e)
{
    (void) e;
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (buttonPressed[x][y])
            {
                handleButtonPress(x, y, false);
            }
        }
    }
}

void MonomeGridDisplay::mouseDrag(const juce::MouseEvent& e)
{
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            bool shouldBePressed = getButtonBounds(x, y).contains(e.getPosition());
            if (shouldBePressed != buttonPressed[x][y])
            {
                handleButtonPress(x, y, shouldBePressed);
            }
        }
    }
}

void MonomeGridDisplay::handleButtonPress(int x, int y, bool down)
{
    buttonPressed[x][y] = down;
    
    if (down)
    {
        DBG("Button pressed: x=" << x << ", y=" << y);
        
        // First row (y=0), columns 4-7: Pattern recorders
        if (y == 0 && x >= 4 && x <= 7)
        {
            DBG("  -> Pattern recorder button detected!");
            int patternIndex = x - 4;  // 0-3 for patterns 0-3
            
            auto* engine = processor.getAudioEngine();
            if (engine)
            {
                auto* pattern = engine->getPattern(patternIndex);
                if (pattern)
                {
                    // Cycle through states: off → recording → playing → off
                    if (pattern->isRecording())
                    {
                        // Recording → Playing: Stop recording and start playback
                        DBG("Pattern " << patternIndex << ": Stop recording, start playback. Events: " << pattern->getEventCount());
                        const double currentBeat = engine->getTimelineBeat();
                        pattern->stopRecording();
                        pattern->startPlayback(currentBeat);
                    }
                    else if (pattern->isPlaying())
                    {
                        // Playing → Off: Stop playback
                        DBG("Pattern " << patternIndex << ": Stop playback");
                        pattern->stopPlayback();
                    }
                    else
                    {
                        // Off → Recording: Start recording
                        DBG("Pattern " << patternIndex << ": Start recording");
                        if (engine)
                            pattern->startRecording(engine->getTimelineBeat());
                    }
                }
            }
        }
        // Rows 0-5: Strip triggering (row 0 = strip 0, row 1 = strip 1, etc.)
        else if (y >= 0 && y < processor.MaxStrips && x < processor.MaxColumns)
        {
            // Skip pattern recorder buttons on row 0, columns 4-7
            if (y == 0 && x >= 4 && x <= 7)
                return;  // Already handled above
            
            int stripIndex = y;  // Row 0 → strip 0, Row 1 → strip 1, etc.
            
            // Trigger the strip
            processor.triggerStrip(stripIndex, x);
        }
    }
    
    // Don't send LEDs to Monome from here - PluginProcessor handles all LED updates
    // This updateFromEngine() is only for updating the GUI visualization
    
    repaint();
}

void MonomeGridDisplay::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateFromEngine();
}

void MonomeGridDisplay::updateFromEngine()
{
    // Update LED states from strips
    // Row 0 = Pattern recorder (columns 4-7)
    // Row 1 = Strip 0
    // Row 2 = Strip 1, etc.
    for (int stripIndex = 0; stripIndex < processor.MaxStrips; ++stripIndex)
    {
        int monomeRow = stripIndex + 1;  // Strip 0 → row 1, Strip 1 → row 2, etc.
        
        if (monomeRow >= gridHeight)
            break;  // Don't exceed grid height
        
        auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
        if (strip)
        {
            // Check if this strip is in Step mode AND control mode is not active
            // When control mode is active (level, pan, sample select, etc.), hide step display
            bool controlModeActive = (processor.getCurrentControlMode() != MlrVSTAudioProcessor::ControlMode::Normal);
            
            if (strip->playMode == EnhancedAudioStrip::PlayMode::Step && !controlModeActive)
            {
                DBG("Strip " << stripIndex << " in Step mode - updating row " << monomeRow);
                
                // Show step pattern on Monome
                const auto visiblePattern = strip->getVisibleStepPattern();
                const int visibleCurrentStep = strip->getVisibleCurrentStep();
                for (int x = 0; x < gridWidth && x < 16; ++x)
                {
                    bool isCurrentStep = (x == visibleCurrentStep);
                    bool isActiveStep = visiblePattern[static_cast<size_t>(x)];
                    
                    int brightness = 0;
                    if (isCurrentStep && isActiveStep)
                    {
                        // Current step AND active - brightest
                        brightness = 15;
                    }
                    else if (isCurrentStep)
                    {
                        // Current step but inactive - medium
                        brightness = 6;
                    }
                    else if (isActiveStep)
                    {
                        // Active step (not current) - medium bright
                        brightness = 10;
                    }
                    else
                    {
                        // Inactive step - dim
                        brightness = 2;
                    }
                    
                    ledState[x][monomeRow] = brightness;
                }
                
                // Debug first few LEDs
                DBG("Step LEDs [0-3]: " << ledState[0][monomeRow] << " " 
                    << ledState[1][monomeRow] << " " 
                    << ledState[2][monomeRow] << " " 
                    << ledState[3][monomeRow]);
            }
            else if (strip->playMode != EnhancedAudioStrip::PlayMode::Step && !controlModeActive)
            {
                // Normal playback mode (Loop/OneShot) - show LED states from strip
                // When control mode is active, PluginProcessor handles ALL LED display
                auto ledStates = strip->getLEDStates();
                for (int x = 0; x < gridWidth && x < processor.MaxColumns; ++x)
                {
                    ledState[x][monomeRow] = ledStates[static_cast<size_t>(x)] ? 12 : 0; // Variable brightness
                }
            }
            // If control mode is active, don't touch LEDs - PluginProcessor handles it
        }
    }
    
    // Row 0, columns 4-7: Pattern recorder status (only if strip 0 NOT in step mode)
    if (gridHeight > 0)
    {
        auto* engine = processor.getAudioEngine();
        if (engine)
        {
            auto* strip0 = engine->getStrip(0);
            bool strip0IsStep = (strip0 && strip0->playMode == EnhancedAudioStrip::PlayMode::Step);
            
            // Only show pattern recorder if strip 0 is not in step mode
            if (!strip0IsStep)
            {
                for (int x = 4; x <= 7 && x < gridWidth; ++x)
                {
                    int patternIndex = x - 4;
                    auto* pattern = engine->getPattern(patternIndex);
                    if (pattern)
                    {
                        if (pattern->isRecording())
                        {
                            // Recording: Bright red (full brightness)
                            ledState[x][0] = 15;
                        }
                        else if (pattern->isPlaying())
                        {
                            // Playing: Medium green
                            ledState[x][0] = 10;
                        }
                        else if (pattern->hasEvents())
                        {
                            // Has recorded pattern: Dim (ready to play)
                            ledState[x][0] = 4;
                        }
                        else
                        {
                            // Empty: Off
                            ledState[x][0] = 0;
                        }
                    }
                }
            }
        }
    }
    
    // Hardware LED writes are centralized in MlrVSTAudioProcessor::updateMonomeLEDs().
    // The editor grid is visualization-only.
    repaint();
}


//==============================================================================
// MonomeControlPanel Implementation
//==============================================================================

MonomeControlPanel::MonomeControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    // Title - compact
    titleLabel.setText("MONOME DEVICE", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));  // Smaller
    titleLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(titleLabel);
    
    // Device selector
    deviceSelector.setTextWhenNoChoicesAvailable("No devices found");
    deviceSelector.setTextWhenNothingSelected("Select device...");
    addAndMakeVisible(deviceSelector);
    
    // Refresh button
    refreshButton.setButtonText("Refresh");
    refreshButton.onClick = [this]() { updateDeviceList(); };
    addAndMakeVisible(refreshButton);
    
    // Connect button
    connectButton.setButtonText("Connect");
    connectButton.onClick = [this]() { connectToDevice(); };
    addAndMakeVisible(connectButton);
    
    // Status label
    statusLabel.setText("Not connected", juce::dontSendNotification);
    statusLabel.setFont(juce::Font(juce::FontOptions(11.0f)));  // Slightly smaller
    statusLabel.setColour(juce::Label::textColourId, kAccent);
    addAndMakeVisible(statusLabel);
    
    // Rotation selector
    rotationLabel.setText("Rotation", juce::dontSendNotification);  // Shorter text
    rotationLabel.setFont(juce::Font(juce::FontOptions(11.0f)));  // Slightly smaller, consistent
    rotationLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(rotationLabel);
    
    rotationSelector.addItem("0°", 1);
    rotationSelector.addItem("90°", 2);
    rotationSelector.addItem("180°", 3);
    rotationSelector.addItem("270°", 4);
    rotationSelector.setSelectedId(1);
    rotationSelector.onChange = [this]()
    {
        int rotation = (rotationSelector.getSelectedId() - 1) * 90;
        processor.getMonomeConnection().setRotation(rotation);
    };
    addAndMakeVisible(rotationSelector);
    
    updateDeviceList();
    startTimer(1000); // Update status every second
}

void MonomeControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void MonomeControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    
    // Compact title
    auto titleRow = bounds.removeFromTop(20);  // Smaller (was 24)
    titleLabel.setBounds(titleRow);
    
    bounds.removeFromTop(6);  // Less gap (was 8)
    
    // Device selector row
    auto deviceRow = bounds.removeFromTop(22);  // Smaller (was 24)
    deviceSelector.setBounds(deviceRow.removeFromLeft(200));
    deviceRow.removeFromLeft(4);
    refreshButton.setBounds(deviceRow.removeFromLeft(70));
    deviceRow.removeFromLeft(4);
    connectButton.setBounds(deviceRow.removeFromLeft(70));
    
    bounds.removeFromTop(6);  // Less gap (was 8)
    
    // Status row
    auto statusRow = bounds.removeFromTop(18);  // Smaller (was 20)
    statusLabel.setBounds(statusRow);
    
    bounds.removeFromTop(6);  // Less gap (was 8)
    
    // Rotation row - ensure it's visible!
    auto rotationRow = bounds.removeFromTop(22);  // Match other controls
    rotationLabel.setBounds(rotationRow.removeFromLeft(70));
    rotationRow.removeFromLeft(4);
    rotationSelector.setBounds(rotationRow.removeFromLeft(100));  // Wider (was 80)
}

void MonomeControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateStatus();
}

void MonomeControlPanel::updateDeviceList()
{
    deviceSelector.clear();
    processor.getMonomeConnection().refreshDeviceList();
    
    auto devices = processor.getMonomeConnection().getDiscoveredDevices();
    for (size_t i = 0; i < devices.size(); ++i)
    {
        auto& device = devices[i];
        juce::String itemText = device.id + " (" + device.type + ") - " +
                                juce::String(device.sizeX) + "x" + juce::String(device.sizeY);
        deviceSelector.addItem(itemText, static_cast<int>(i + 1));
    }
    
    if (devices.size() > 0)
        deviceSelector.setSelectedId(1);
}

void MonomeControlPanel::connectToDevice()
{
    int selectedIndex = deviceSelector.getSelectedId() - 1;
    if (selectedIndex >= 0)
    {
        processor.getMonomeConnection().selectDevice(selectedIndex);
    }
}

void MonomeControlPanel::updateStatus()
{
    auto status = processor.getMonomeConnection().getConnectionStatus();
    statusLabel.setText(status, juce::dontSendNotification);
    
    bool connected = processor.getMonomeConnection().isConnected();
    statusLabel.setColour(juce::Label::textColourId,
                          connected ? juce::Colour(0xff76be7e) : kAccent);
}


//==============================================================================
// GlobalControlPanel Implementation
//==============================================================================

GlobalControlPanel::GlobalControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    // Title - compact
    titleLabel.setText("GLOBAL CONTROLS", juce::dontSendNotification);  // Uppercase, compact
    titleLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(titleLabel);
    titleLabel.setTooltip("Master timing, quality, monitoring, and UI help settings.");

    versionLabel.setText("v" + juce::String(JucePlugin_VersionString), juce::dontSendNotification);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    versionLabel.setColour(juce::Label::textColourId, kTextMuted);
    versionLabel.setTooltip("Plugin version.");
    addAndMakeVisible(versionLabel);
    
    // Master volume
    masterVolumeLabel.setText("Master", juce::dontSendNotification);
    masterVolumeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(masterVolumeLabel);
    
    masterVolumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    masterVolumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);  // Clean, no text
    masterVolumeSlider.setRange(0.0, 1.0, 0.01);
    masterVolumeSlider.setValue(1.0);
    enableAltClickReset(masterVolumeSlider, 1.0);
    masterVolumeSlider.setPopupDisplayEnabled(true, false, this);  // Show value on hover
    addAndMakeVisible(masterVolumeSlider);
    
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "masterVolume", masterVolumeSlider);
    masterVolumeSlider.onDragEnd = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    limiterToggle.setButtonText("Limiter");
    limiterToggle.setClickingTogglesState(true);
    limiterToggle.setTooltip("Enable JUCE limiter on plugin outputs.");
    addAndMakeVisible(limiterToggle);
    styleUiButton(limiterToggle);
    limiterEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "limiterEnabled", limiterToggle);
    limiterToggle.onClick = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    
    // Quantize
    quantizeLabel.setText("Quantize", juce::dontSendNotification);
    quantizeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(quantizeLabel);
    
    quantizeSelector.addItem("1", 1);
    quantizeSelector.addItem("1/2", 2);
    quantizeSelector.addItem("1/2T", 3);
    quantizeSelector.addItem("1/4", 4);
    quantizeSelector.addItem("1/4T", 5);
    quantizeSelector.addItem("1/8", 6);
    quantizeSelector.addItem("1/8T", 7);
    quantizeSelector.addItem("1/16", 8);
    quantizeSelector.addItem("1/16T", 9);
    quantizeSelector.addItem("1/32", 10);
    quantizeSelector.setSelectedId(6);  // Default to 1/8
    addAndMakeVisible(quantizeSelector);
    styleUiCombo(quantizeSelector);
    quantizeSelector.setTooltip("Global trigger quantization grid.");
    
    quantizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "quantize", quantizeSelector);
    quantizeSelector.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    innerLoopLengthLabel.setText("Inner Loop", juce::dontSendNotification);
    innerLoopLengthLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(innerLoopLengthLabel);

    innerLoopLengthBox.addItem("1", 1);
    innerLoopLengthBox.addItem("1/2", 2);
    innerLoopLengthBox.addItem("1/4", 3);
    innerLoopLengthBox.addItem("1/8", 4);
    innerLoopLengthBox.addItem("1/16", 5);
    innerLoopLengthBox.setSelectedId(1);
    addAndMakeVisible(innerLoopLengthBox);
    styleUiCombo(innerLoopLengthBox);
    innerLoopLengthBox.setTooltip("Divides inner-loop size selected from the monome grid.");

    innerLoopLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "innerLoopLength", innerLoopLengthBox);
    innerLoopLengthBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    swingDivisionLabel.setText("Swing grid", juce::dontSendNotification);
    swingDivisionLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(swingDivisionLabel);

    swingDivisionBox.addItem("1/4", 1);
    swingDivisionBox.addItem("1/8", 2);
    swingDivisionBox.addItem("1/16", 3);
    swingDivisionBox.addItem("1/8T", 4);
    swingDivisionBox.addItem("1/2", 5);
    swingDivisionBox.addItem("1/32", 6);
    swingDivisionBox.addItem("1/16T", 7);
    swingDivisionBox.onChange = [this]()
    {
        processor.setSwingDivisionSelection(swingDivisionBox.getSelectedId() - 1);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    addAndMakeVisible(swingDivisionBox);
    styleUiCombo(swingDivisionBox);
    swingDivisionBox.setTooltip("Swing subdivision grid. 1/8T is triplet swing (3 subdivisions per beat), 1/16T is 6 subdivisions per beat.");

    outputRoutingLabel.setText("Outputs", juce::dontSendNotification);
    outputRoutingLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(outputRoutingLabel);

    outputRoutingBox.addItem("Stereo Mix", 1);
    outputRoutingBox.addItem("Separate Strip Outs", 2);
    outputRoutingBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(outputRoutingBox);
    styleUiCombo(outputRoutingBox);
    outputRoutingBox.setTooltip("Route strip audio to separate DAW outputs (requires multi-output plugin instance).");
    outputRoutingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "outputRouting", outputRoutingBox);
    outputRoutingBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    pitchControlModeLabel.setText("Pitch Mode", juce::dontSendNotification);
    pitchControlModeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchControlModeLabel);

    pitchControlModeBox.addItem("Pitch Shift", 1);
    pitchControlModeBox.addItem("Resample", 2);
    pitchControlModeBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(pitchControlModeBox);
    styleUiCombo(pitchControlModeBox);
    pitchControlModeBox.setTooltip("Global strip pitch behavior: legacy pitch shift or playback-rate resample.");
    pitchControlModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "pitchControlMode", pitchControlModeBox);
    pitchControlModeBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    
    // Grain quality (global for all strips in Grain mode)
    qualityLabel.setText("Grain Q", juce::dontSendNotification);
    qualityLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(qualityLabel);
    
    resamplingQualityBox.addItem("Linear", 1);
    resamplingQualityBox.addItem("Cubic", 2);
    resamplingQualityBox.addItem("Sinc", 3);
    resamplingQualityBox.addItem("Sinc HQ", 4);
    resamplingQualityBox.setSelectedId(3);
    addAndMakeVisible(resamplingQualityBox);
    styleUiCombo(resamplingQualityBox);
    resamplingQualityBox.setTooltip("Global grain interpolation quality for all strips.");
    grainQualityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "quality", resamplingQualityBox);
    resamplingQualityBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    
    // Input monitoring
    inputMonitorLabel.setText("Input", juce::dontSendNotification);
    inputMonitorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(inputMonitorLabel);
    
    inputMonitorSlider.setSliderStyle(juce::Slider::LinearVertical);
    inputMonitorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);  // Clean
    inputMonitorSlider.setRange(0.0, 1.0, 0.01);
    inputMonitorSlider.setValue(0.0);
    enableAltClickReset(inputMonitorSlider, 1.0);
    inputMonitorSlider.setPopupDisplayEnabled(true, false, this);  // Show on hover
    addAndMakeVisible(inputMonitorSlider);
    inputMonitorSlider.setTooltip("Monitor live input signal level.");

    inputMonitorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "inputMonitor", inputMonitorSlider);
    inputMonitorSlider.onDragEnd = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    
    // Input meters
    inputMeterLabel.setText("L   R", juce::dontSendNotification);
    inputMeterLabel.setJustificationType(juce::Justification::centred);
    inputMeterLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    addAndMakeVisible(inputMeterLabel);
    
    addAndMakeVisible(inputMeterL);
    addAndMakeVisible(inputMeterR);
    
    // Loop crossfade length
    crossfadeLengthLabel.setText("Crossfade", juce::dontSendNotification);
    crossfadeLengthLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(crossfadeLengthLabel);
    
    crossfadeLengthSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    crossfadeLengthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    crossfadeLengthSlider.setRange(1.0, 50.0, 0.1);
    crossfadeLengthSlider.setValue(10.0);
    enableAltClickReset(crossfadeLengthSlider, 10.0);
    crossfadeLengthSlider.setPopupDisplayEnabled(true, false, this);
    crossfadeLengthSlider.setTextValueSuffix(" ms");
    addAndMakeVisible(crossfadeLengthSlider);
    crossfadeLengthSlider.setTooltip("Loop/capture crossfade time in milliseconds.");

    triggerFadeInLabel.setText("Trig Fade", juce::dontSendNotification);
    triggerFadeInLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(triggerFadeInLabel);

    triggerFadeInSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    triggerFadeInSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    triggerFadeInSlider.setRange(0.1, 120.0, 0.1);
    triggerFadeInSlider.setValue(12.0);
    enableAltClickReset(triggerFadeInSlider, 12.0);
    triggerFadeInSlider.setPopupDisplayEnabled(true, false, this);
    triggerFadeInSlider.setTextValueSuffix(" ms");
    addAndMakeVisible(triggerFadeInSlider);
    triggerFadeInSlider.setTooltip("Fade-in time for Monome row strip triggers.");

    tooltipsToggle.setButtonText("Tooltips");
    tooltipsToggle.setClickingTogglesState(true);
    tooltipsToggle.setToggleState(false, juce::dontSendNotification);
    tooltipsToggle.setTooltip("Show or hide control descriptions on mouse hover.");
    tooltipsToggle.onClick = [this]()
    {
        if (onTooltipsToggled)
            onTooltipsToggled(tooltipsToggle.getToggleState());
    };
    addAndMakeVisible(tooltipsToggle);
    styleUiButton(tooltipsToggle);

    momentaryToggle.setButtonText("Momentary");
    momentaryToggle.setClickingTogglesState(true);
    momentaryToggle.onClick = [this]()
    {
        processor.setControlPageMomentary(momentaryToggle.getToggleState());
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    momentaryToggle.setTooltip("Monome page buttons are hold-to-temporary when enabled.");
    addAndMakeVisible(momentaryToggle);
    styleUiButton(momentaryToggle);

    soundTouchToggle.setButtonText("SoundTouch");
    soundTouchToggle.setClickingTogglesState(true);
    soundTouchToggle.setTooltip("Enable SoundTouch swing warp in Loop/Gate playback.");
    addAndMakeVisible(soundTouchToggle);
    styleUiButton(soundTouchToggle);
    soundTouchEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "soundTouchEnabled", soundTouchToggle);
    soundTouchToggle.onClick = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    
    crossfadeLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "crossfadeLength", crossfadeLengthSlider);
    triggerFadeInAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "triggerFadeIn", triggerFadeInSlider);
    crossfadeLengthSlider.onDragEnd = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };
    triggerFadeInSlider.onDragEnd = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    refreshFromProcessor();
    globalUiReady = true;
}

//==============================================================================
// PresetControlPanel Implementation
//==============================================================================

PresetControlPanel::PresetControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    instructionsLabel.setText("Click=load default/preset  Shift+Click=save  Right-click=delete", juce::dontSendNotification);
    instructionsLabel.setJustificationType(juce::Justification::centredLeft);
    instructionsLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(instructionsLabel);

    presetNameEditor.setTextToShowWhenEmpty("Preset name", kTextMuted);
    presetNameEditor.setMultiLine(false);
    presetNameEditor.setReturnKeyStartsNewLine(false);
    presetNameEditor.setSelectAllWhenFocused(true);
    presetNameEditor.setMouseClickGrabsKeyboardFocus(true);
    presetNameEditor.onTextChange = [this]() { presetNameDraft = presetNameEditor.getText(); };
    presetNameEditor.onFocusLost = [this]()
    {
        presetNameDraft = presetNameEditor.getText();
    };
    presetNameEditor.onReturnKey = [this]()
    {
        savePresetClicked(selectedPresetIndex, presetNameEditor.getText());
    };
    addAndMakeVisible(presetNameEditor);

    saveButton.setButtonText("Save");
    saveButton.onClick = [this]()
    {
        auto safePanel = juce::Component::SafePointer<PresetControlPanel>(this);
        // Defer one message tick so in-flight text edits are committed first.
        juce::MessageManager::callAsync([safe = safePanel]()
        {
            if (safe == nullptr)
                return;
            safe->savePresetClicked(safe->selectedPresetIndex, safe->presetNameEditor.getText());
        });
    };
    addAndMakeVisible(saveButton);
    styleUiButton(saveButton, true);

    deleteButton.setButtonText("Delete");
    deleteButton.onClick = [this]()
    {
        if (processor.deletePreset(selectedPresetIndex))
            updatePresetButtons();
    };
    addAndMakeVisible(deleteButton);
    styleUiButton(deleteButton);

    exportWavButton.setButtonText("Export");
    exportWavButton.onClick = [this]() { exportRecordingsAsWav(); };
    exportWavButton.setTooltip("Export current strip recordings to WAV files.");
    addAndMakeVisible(exportWavButton);
    styleUiButton(exportWavButton);

    presetViewport.setViewedComponent(&presetGridContent, false);
    presetViewport.setScrollBarsShown(true, true, true, true);
    presetViewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible(presetViewport);

    // 16x7 preset grid, origin 0x0
    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        const int x = i % MlrVSTAudioProcessor::PresetColumns;
        const int y = i / MlrVSTAudioProcessor::PresetColumns;
        auto& button = presetButtons[static_cast<size_t>(i)];
        button.setButtonText(juce::String(x) + "," + juce::String(y));
        button.setClickingTogglesState(false);
        styleUiButton(button);
        button.addMouseListener(this, false);

        button.onClick = [this, i]()
        {
            if (juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown())
            {
                auto safePanel = juce::Component::SafePointer<PresetControlPanel>(this);
                juce::MessageManager::callAsync([safe = safePanel, i]()
                {
                    if (safe == nullptr)
                        return;
                    safe->savePresetClicked(i, safe->presetNameEditor.getText());
                });
            }
            else
                loadPresetClicked(i);
        };
        button.setTooltip("Preset " + juce::String(i + 1) + " (" + juce::String(x) + "," + juce::String(y) + ")");
        presetGridContent.addAndMakeVisible(button);
    }
    
    selectedPresetIndex = juce::jmax(0, processor.getLoadedPresetIndex());
    presetNameDraft = processor.getPresetName(selectedPresetIndex);
    presetNameEditor.setText(presetNameDraft, juce::dontSendNotification);
    layoutPresetButtons();
    updatePresetButtons();
}

void PresetControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void PresetControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    auto editorArea = bounds.removeFromTop(26);
    const int saveDeleteButtonW = 60;
    const int exportButtonW = 78;
    deleteButton.setBounds(editorArea.removeFromRight(saveDeleteButtonW));
    editorArea.removeFromRight(4);
    exportWavButton.setBounds(editorArea.removeFromRight(exportButtonW));
    editorArea.removeFromRight(4);
    saveButton.setBounds(editorArea.removeFromRight(saveDeleteButtonW));
    editorArea.removeFromRight(6);

    // Keep name input compact to protect button layout on narrow widths.
    constexpr int kNameFieldMaxW = 180;
    const int nameW = juce::jmin(kNameFieldMaxW, editorArea.getWidth());
    presetNameEditor.setBounds(editorArea.removeFromLeft(nameW));
    editorArea.removeFromLeft(6);
    instructionsLabel.setBounds(editorArea);
    bounds.removeFromTop(2);

    presetViewport.setBounds(bounds);
    layoutPresetButtons();
}

void PresetControlPanel::mouseUp(const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown())
        return;

    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        auto& button = presetButtons[static_cast<size_t>(i)];
        if (e.originalComponent == &button || e.eventComponent == &button)
        {
            selectedPresetIndex = i;
            if (processor.deletePreset(i))
            {
                presetNameDraft = processor.getPresetName(i);
                presetNameEditor.setText(presetNameDraft, juce::dontSendNotification);
                updatePresetButtons();
            }
            break;
        }
    }
}

void PresetControlPanel::savePresetClicked(int index, juce::String typedName)
{
    processor.savePreset(index);
    const auto trimmed = (typedName.isNotEmpty() ? typedName : presetNameEditor.getText()).trim();
    if (trimmed.isNotEmpty())
    {
        processor.setPresetName(index, trimmed);
        presetNameDraft = trimmed;
        presetNameEditor.setText(trimmed, juce::dontSendNotification);
    }
    selectedPresetIndex = index;
    updatePresetButtons();
}

void PresetControlPanel::loadPresetClicked(int index)
{
    processor.loadPreset(index);
    selectedPresetIndex = index;
    const auto name = processor.getPresetName(index);
    presetNameDraft = name;
    presetNameEditor.setText(name, juce::dontSendNotification);
}

void PresetControlPanel::exportRecordingsAsWav()
{
    auto startDir = lastExportDirectory;
    if (!startDir.exists() || !startDir.isDirectory())
        startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    juce::FileChooser chooser("Export strip recordings to folder", startDir, "*");
    if (!chooser.browseForDirectory())
        return;

    auto targetDir = chooser.getResult();
    if (!targetDir.exists())
        targetDir.createDirectory();
    lastExportDirectory = targetDir;

    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    int exportedCount = 0;
    int failedCount = 0;
    juce::WavAudioFormat wavFormat;

    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        auto* strip = engine->getStrip(i);
        if (strip == nullptr || !strip->hasAudio())
            continue;

        const auto* audioBuffer = strip->getAudioBuffer();
        const double sampleRate = strip->getSourceSampleRate();
        if (audioBuffer == nullptr || audioBuffer->getNumSamples() <= 0 || sampleRate <= 1000.0)
            continue;

        auto outFile = targetDir.getChildFile("Strip_" + juce::String(i + 1) + ".wav");
        std::unique_ptr<juce::FileOutputStream> outStream(outFile.createOutputStream());
        if (outStream == nullptr)
        {
            ++failedCount;
            continue;
        }

        auto writerStream = std::unique_ptr<juce::OutputStream>(outStream.release());
        const auto writerOptions = juce::AudioFormatWriter::Options{}
            .withSampleRate(sampleRate)
            .withNumChannels(audioBuffer->getNumChannels())
            .withBitsPerSample(24)
            .withQualityOptionIndex(0);
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(writerStream, writerOptions));

        if (writer == nullptr || !writer->writeFromAudioSampleBuffer(*audioBuffer, 0, audioBuffer->getNumSamples()))
        {
            ++failedCount;
            continue;
        }

        writer->flush();
        ++exportedCount;
    }

    const juce::String message = "Exported " + juce::String(exportedCount)
        + " strip recording(s) to:\n" + targetDir.getFullPathName()
        + (failedCount > 0 ? "\nFailed: " + juce::String(failedCount) : "");
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Export WAV", message);
}

void PresetControlPanel::updatePresetButtons()
{
    const int loadedPreset = processor.getLoadedPresetIndex();
    deleteButton.setEnabled(processor.presetExists(selectedPresetIndex));
    auto shortPresetLabel = [](const juce::String& name, int fallbackIndex) -> juce::String
    {
        auto n = name.trim();
        if (n.isEmpty())
            return juce::String(fallbackIndex + 1);
        juce::String compact;
        for (auto c : n)
        {
            if (!juce::CharacterFunctions::isWhitespace(c))
                compact << juce::String::charToString(c);
            if (compact.length() >= 4)
                break;
        }
        if (compact.isEmpty())
            compact = juce::String(fallbackIndex + 1);
        return compact.toUpperCase();
    };

    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        bool exists = processor.presetExists(i);
        auto& button = presetButtons[static_cast<size_t>(i)];
        const juce::String presetName = exists ? processor.getPresetName(i) : juce::String();
        button.setButtonText(shortPresetLabel(presetName, i));
        juce::String tip = "Preset " + juce::String(i + 1);
        if (exists)
            tip << " - " << presetName;
        button.setTooltip(tip);
        if (i == loadedPreset && exists)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffb8d478));
            button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff111111));
        }
        else
        {
            const bool isSelected = (i == selectedPresetIndex);
            button.setColour(juce::TextButton::buttonColourId,
                             exists
                                 ? (isSelected ? kAccent.withMultipliedBrightness(1.1f) : kAccent.withMultipliedBrightness(0.9f))
                                 : (isSelected ? juce::Colour(0xff3a3a3a) : juce::Colour(0xff2b2b2b)));
            button.setColour(juce::TextButton::textColourOffId,
                             exists ? juce::Colour(0xfff3f3f3) : kTextMuted);
        }
    }
}

void PresetControlPanel::layoutPresetButtons()
{
    const int gap = 4;
    const int buttonHeight = 16;
    const int minButtonWidth = 26;

    const int viewportWidth = juce::jmax(0, presetViewport.getWidth() - presetViewport.getScrollBarThickness());
    const int buttonWidth = juce::jmax(minButtonWidth,
                                       (viewportWidth - ((MlrVSTAudioProcessor::PresetColumns - 1) * gap))
                                       / MlrVSTAudioProcessor::PresetColumns);
    const int contentWidth = (MlrVSTAudioProcessor::PresetColumns * buttonWidth)
                             + ((MlrVSTAudioProcessor::PresetColumns - 1) * gap);
    const int contentHeight = (MlrVSTAudioProcessor::PresetRows * buttonHeight)
                              + ((MlrVSTAudioProcessor::PresetRows - 1) * gap);

    presetGridContent.setSize(contentWidth, contentHeight);

    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        const int x = i % MlrVSTAudioProcessor::PresetColumns;
        const int y = i / MlrVSTAudioProcessor::PresetColumns;
        presetButtons[static_cast<size_t>(i)].setBounds(x * (buttonWidth + gap),
                                                        y * (buttonHeight + gap),
                                                        buttonWidth,
                                                        buttonHeight);
    }
}

void PresetControlPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int deltaY = static_cast<int>(-wheel.deltaY * 96.0f);
    if (deltaY != 0)
        presetViewport.setViewPosition(presetViewport.getViewPositionX(),
                                       juce::jmax(0, presetViewport.getViewPositionY() + deltaY));
}

void PresetControlPanel::refreshVisualState()
{
    updatePresetButtons();
}

//==============================================================================
// PathsControlPanel Implementation
//==============================================================================

PathsControlPanel::PathsControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    titleLabel.setText("DEFAULT LOAD PATHS", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    scrollViewport.setViewedComponent(&scrollContent, false);
    scrollViewport.setScrollBarsShown(true, false, true, true);
    addAndMakeVisible(scrollViewport);

    headerStripLabel.setText("Strip", juce::dontSendNotification);
    headerStripLabel.setColour(juce::Label::textColourId, kTextMuted);
    headerStripLabel.setJustificationType(juce::Justification::centredLeft);
    scrollContent.addAndMakeVisible(headerStripLabel);

    headerLoopLabel.setText("Loop Mode Path", juce::dontSendNotification);
    headerLoopLabel.setColour(juce::Label::textColourId, kTextMuted);
    headerLoopLabel.setJustificationType(juce::Justification::centredLeft);
    scrollContent.addAndMakeVisible(headerLoopLabel);

    headerStepLabel.setText("Step Mode Path", juce::dontSendNotification);
    headerStepLabel.setColour(juce::Label::textColourId, kTextMuted);
    headerStepLabel.setJustificationType(juce::Justification::centredLeft);
    scrollContent.addAndMakeVisible(headerStepLabel);

    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        auto& row = rows[static_cast<size_t>(i)];

        row.stripLabel.setText("S" + juce::String(i + 1), juce::dontSendNotification);
        row.stripLabel.setColour(juce::Label::textColourId, getStripColor(i));
        row.stripLabel.setJustificationType(juce::Justification::centredLeft);
        scrollContent.addAndMakeVisible(row.stripLabel);

        row.loopPathLabel.setColour(juce::Label::textColourId, kTextPrimary);
        row.loopPathLabel.setJustificationType(juce::Justification::centredLeft);
        scrollContent.addAndMakeVisible(row.loopPathLabel);

        row.loopSetButton.setButtonText("Set");
        row.loopSetButton.setTooltip("Set default loop-mode sample folder.");
        row.loopSetButton.onClick = [this, i]() { chooseDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Loop); };
        scrollContent.addAndMakeVisible(row.loopSetButton);

        row.loopClearButton.setButtonText("Clear");
        row.loopClearButton.setTooltip("Clear default loop-mode folder.");
        row.loopClearButton.onClick = [this, i]() { clearDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Loop); };
        scrollContent.addAndMakeVisible(row.loopClearButton);

        row.stepPathLabel.setColour(juce::Label::textColourId, kTextPrimary);
        row.stepPathLabel.setJustificationType(juce::Justification::centredLeft);
        scrollContent.addAndMakeVisible(row.stepPathLabel);

        row.stepSetButton.setButtonText("Set");
        row.stepSetButton.setTooltip("Set default step-mode sample folder.");
        row.stepSetButton.onClick = [this, i]() { chooseDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Step); };
        scrollContent.addAndMakeVisible(row.stepSetButton);

        row.stepClearButton.setButtonText("Clear");
        row.stepClearButton.setTooltip("Clear default step-mode folder.");
        row.stepClearButton.onClick = [this, i]() { clearDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Step); };
        scrollContent.addAndMakeVisible(row.stepClearButton);
    }

    refreshLabels();
    startTimer(500);
}

void PathsControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void PathsControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    titleLabel.setBounds(bounds.removeFromTop(18));
    bounds.removeFromTop(6);
    scrollViewport.setBounds(bounds);

    const int rowHeight = 24;
    const int contentHeight = 18 + 4 + (rowHeight * MlrVSTAudioProcessor::MaxStrips);
    const int contentWidth = juce::jmax(200, scrollViewport.getWidth() - scrollViewport.getScrollBarThickness());
    scrollContent.setSize(contentWidth, contentHeight);

    auto layout = scrollContent.getLocalBounds();

    auto header = layout.removeFromTop(18);
    const int stripWidth = 42;
    const int buttonWidth = 48;
    const int gap = 4;
    const int pathAreaWidth = (header.getWidth() - stripWidth - (4 * buttonWidth) - (6 * gap)) / 2;

    headerStripLabel.setBounds(header.removeFromLeft(stripWidth));
    header.removeFromLeft(gap);
    headerLoopLabel.setBounds(header.removeFromLeft(pathAreaWidth + (2 * buttonWidth) + (2 * gap)));
    header.removeFromLeft(gap);
    headerStepLabel.setBounds(header);

    layout.removeFromTop(4);

    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        auto& row = rows[static_cast<size_t>(i)];
        auto rowArea = layout.removeFromTop(rowHeight);
        rowArea.removeFromBottom(2);

        row.stripLabel.setBounds(rowArea.removeFromLeft(stripWidth));
        rowArea.removeFromLeft(gap);

        row.loopPathLabel.setBounds(rowArea.removeFromLeft(pathAreaWidth));
        rowArea.removeFromLeft(gap);
        row.loopSetButton.setBounds(rowArea.removeFromLeft(buttonWidth));
        rowArea.removeFromLeft(gap);
        row.loopClearButton.setBounds(rowArea.removeFromLeft(buttonWidth));
        rowArea.removeFromLeft(gap * 2);

        row.stepPathLabel.setBounds(rowArea.removeFromLeft(pathAreaWidth));
        rowArea.removeFromLeft(gap);
        row.stepSetButton.setBounds(rowArea.removeFromLeft(buttonWidth));
        rowArea.removeFromLeft(gap);
        row.stepClearButton.setBounds(rowArea.removeFromLeft(buttonWidth));
    }
}

void PathsControlPanel::timerCallback()
{
    refreshLabels();
}

void PathsControlPanel::refreshLabels()
{
    for (int i = 0; i < MlrVSTAudioProcessor::MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopDir = processor.getDefaultSampleDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Loop);
        const auto stepDir = processor.getDefaultSampleDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Step);

        rows[idx].loopPathLabel.setText(pathToDisplay(loopDir), juce::dontSendNotification);
        rows[idx].loopPathLabel.setTooltip(loopDir.getFullPathName());
        rows[idx].stepPathLabel.setText(pathToDisplay(stepDir), juce::dontSendNotification);
        rows[idx].stepPathLabel.setTooltip(stepDir.getFullPathName());
    }
}

void PathsControlPanel::chooseDirectory(int stripIndex, MlrVSTAudioProcessor::SamplePathMode mode)
{
    auto startDir = processor.getDefaultSampleDirectory(stripIndex, mode);
    if (!startDir.exists() || !startDir.isDirectory())
        startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    const juce::String modeName = (mode == MlrVSTAudioProcessor::SamplePathMode::Step) ? "Step" : "Loop";
    juce::FileChooser chooser("Select " + modeName + " Default Path for Strip " + juce::String(stripIndex + 1),
                              startDir,
                              "*");

    if (chooser.browseForDirectory())
    {
        processor.setDefaultSampleDirectory(stripIndex, mode, chooser.getResult());
        refreshLabels();
    }
}

void PathsControlPanel::clearDirectory(int stripIndex, MlrVSTAudioProcessor::SamplePathMode mode)
{
    processor.setDefaultSampleDirectory(stripIndex, mode, {});
    refreshLabels();
}

juce::String PathsControlPanel::pathToDisplay(const juce::File& file)
{
    if (!file.exists() || !file.isDirectory())
        return "(not set)";
    return file.getFullPathName();
}

void GlobalControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void GlobalControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(6);  // Less padding (was 8)
    
    auto titleRow = bounds.removeFromTop(20);  // Smaller title (was 24)
    tooltipsToggle.setBounds(titleRow.removeFromRight(86));
    titleRow.removeFromRight(6);
    soundTouchToggle.setBounds(titleRow.removeFromRight(102));
    titleRow.removeFromRight(6);
    momentaryToggle.setBounds(titleRow.removeFromRight(92));
    titleRow.removeFromRight(6);
    versionLabel.setBounds(titleRow.removeFromRight(62));
    titleRow.removeFromRight(6);
    titleLabel.setBounds(titleRow);
    
    bounds.removeFromTop(4);  // Less gap (was 8)
    
    auto controlsArea = bounds;
    
    // COMPACT LAYOUT: Reduce individual column widths
    // Calculate based on actual needs rather than equal division
    const int sliderWidth = 48;      // Vertical sliders
    const int meterWidth = 28;       // L/R meters
    const int knobWidth = 100;       // Rotary fade controls
    const int dropdownWidth = 84;    // Dropdowns
    const int spacing = 6;           // Between controls
    
    // Master volume
    auto masterArea = controlsArea.removeFromLeft(sliderWidth);
    masterVolumeLabel.setBounds(masterArea.removeFromTop(16));  // Smaller label (was 20)
    masterArea.removeFromTop(2);  // Less gap (was 4)
    masterVolumeSlider.setBounds(masterArea);
    controlsArea.removeFromLeft(spacing);

    auto limiterArea = controlsArea.removeFromLeft(84);
    limiterToggle.setBounds(limiterArea.removeFromTop(24));
    controlsArea.removeFromLeft(spacing);
    
    // Input monitor slider
    auto inputArea = controlsArea.removeFromLeft(sliderWidth);
    inputMonitorLabel.setBounds(inputArea.removeFromTop(16));
    inputArea.removeFromTop(2);
    inputMonitorSlider.setBounds(inputArea);
    controlsArea.removeFromLeft(spacing);
    
    // Input meters (L/R) - compact
    auto meterArea = controlsArea.removeFromLeft(meterWidth);
    inputMeterLabel.setBounds(meterArea.removeFromTop(16));
    meterArea.removeFromTop(2);
    auto halfMeter = meterArea.getWidth() / 2;
    inputMeterL.setBounds(meterArea.removeFromLeft(halfMeter).reduced(1));
    inputMeterR.setBounds(meterArea.reduced(1));
    controlsArea.removeFromLeft(spacing);
    
    // Crossfade length - compact knob
    auto crossfadeArea = controlsArea.removeFromLeft(knobWidth);
    crossfadeLengthLabel.setBounds(crossfadeArea.removeFromTop(16));
    crossfadeArea.removeFromTop(2);
    crossfadeLengthSlider.setBounds(crossfadeArea.removeFromTop(104));
    controlsArea.removeFromLeft(2);

    // Trigger fade-in - compact knob
    auto triggerFadeArea = controlsArea.removeFromLeft(knobWidth);
    triggerFadeInLabel.setBounds(triggerFadeArea.removeFromTop(16));
    triggerFadeArea.removeFromTop(2);
    triggerFadeInSlider.setBounds(triggerFadeArea.removeFromTop(104));
    controlsArea.removeFromLeft(spacing);
    
    // Quantize - compact dropdown
    auto quantizeArea = controlsArea.removeFromLeft(dropdownWidth);
    quantizeLabel.setBounds(quantizeArea.removeFromTop(16));
    quantizeArea.removeFromTop(2);
    quantizeSelector.setBounds(quantizeArea.removeFromTop(28));
    controlsArea.removeFromLeft(spacing);

    // Inner loop length divisor
    auto innerLoopArea = controlsArea.removeFromLeft(dropdownWidth);
    innerLoopLengthLabel.setBounds(innerLoopArea.removeFromTop(16));
    innerLoopArea.removeFromTop(2);
    innerLoopLengthBox.setBounds(innerLoopArea.removeFromTop(28));
    controlsArea.removeFromLeft(spacing);

    // Quality - compact dropdown
    auto qualityArea = controlsArea.removeFromLeft(dropdownWidth);
    qualityLabel.setBounds(qualityArea.removeFromTop(16));
    qualityArea.removeFromTop(2);
    resamplingQualityBox.setBounds(qualityArea.removeFromTop(28));
    controlsArea.removeFromLeft(spacing);

    // Swing grid - compact dropdown
    auto swingArea = controlsArea.removeFromLeft(dropdownWidth);
    swingDivisionLabel.setBounds(swingArea.removeFromTop(16));
    swingArea.removeFromTop(2);
    swingDivisionBox.setBounds(swingArea.removeFromTop(28));
    controlsArea.removeFromLeft(spacing);

    auto pitchModeArea = controlsArea.removeFromLeft(dropdownWidth);
    pitchControlModeLabel.setBounds(pitchModeArea.removeFromTop(16));
    pitchModeArea.removeFromTop(2);
    pitchControlModeBox.setBounds(pitchModeArea.removeFromTop(28));
    controlsArea.removeFromLeft(spacing);

    auto outputRoutingArea = controlsArea.removeFromLeft(120);
    outputRoutingLabel.setBounds(outputRoutingArea.removeFromTop(16));
    outputRoutingArea.removeFromTop(2);
    outputRoutingBox.setBounds(outputRoutingArea.removeFromTop(28));
}

void GlobalControlPanel::updateMeters(float leftLevel, float rightLevel)
{
    inputMeterL.setLevel(leftLevel);
    inputMeterR.setLevel(rightLevel);
}

void GlobalControlPanel::refreshFromProcessor()
{
    swingDivisionBox.setSelectedId(processor.getSwingDivisionSelection() + 1, juce::dontSendNotification);
    momentaryToggle.setToggleState(processor.isControlPageMomentary(), juce::dontSendNotification);
}

//==============================================================================
// MonomePagesPanel Implementation
//==============================================================================

MonomePagesPanel::MonomePagesPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    for (int i = 0; i < MlrVSTAudioProcessor::NumControlRowPages; ++i)
    {
        auto& row = rows[static_cast<size_t>(i)];
        row.positionLabel.setJustificationType(juce::Justification::centred);
        row.positionLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        row.positionLabel.setColour(juce::Label::textColourId, kTextMuted);
        addAndMakeVisible(row.positionLabel);

        row.modeButton.setClickingTogglesState(false);
        row.modeButton.setTriggeredOnMouseDown(true);
        styleUiButton(row.modeButton);
        row.modeButton.setTooltip("Click to activate this page");
        row.modeButton.onStateChange = [this, i]()
        {
            if (!processor.isControlPageMomentary())
                return;
            const auto modeAtButton = processor.getControlModeForControlButton(i);
            const bool isDown = rows[static_cast<size_t>(i)].modeButton.isDown();
            processor.setControlModeFromGui(isDown ? modeAtButton : MlrVSTAudioProcessor::ControlMode::Normal,
                                            isDown);
            refreshFromProcessor();
        };
        row.modeButton.onClick = [this, i]()
        {
            if (processor.isControlPageMomentary())
                return; // handled by onStateChange while pressed
            const auto modeAtButton = processor.getControlModeForControlButton(i);
            const bool active = processor.isControlModeActive()
                                && processor.getCurrentControlMode() == modeAtButton;
            processor.setControlModeFromGui(active ? MlrVSTAudioProcessor::ControlMode::Normal
                                                   : modeAtButton,
                                            !active);
            refreshFromProcessor();
        };
        addAndMakeVisible(row.modeButton);

        row.upButton.setButtonText("^");
        row.upButton.setTooltip("Move page left");
        row.upButton.onClick = [this, i]()
        {
            processor.moveControlPage(i, i - 1);
            refreshFromProcessor();
        };
        addAndMakeVisible(row.upButton);
        styleUiButton(row.upButton);

        row.downButton.setButtonText("v");
        row.downButton.setTooltip("Move page right");
        row.downButton.onClick = [this, i]()
        {
            processor.moveControlPage(i, i + 1);
            refreshFromProcessor();
        };
        addAndMakeVisible(row.downButton);
        styleUiButton(row.downButton);
    }

    refreshFromProcessor();
    startTimer(200);
}

void MonomePagesPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);

    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(4);

    auto pageOrderArea = bounds.removeFromTop(58);
    const int numSlots = MlrVSTAudioProcessor::NumControlRowPages;
    const int gapX = 4;
    const int slotWidth = juce::jmax(52, (pageOrderArea.getWidth() - ((numSlots - 1) * gapX)) / juce::jmax(1, numSlots));
    const int slotHeight = pageOrderArea.getHeight();

    g.setColour(juce::Colour(0xff2a2a2a).withAlpha(0.9f));
    for (int i = 0; i < numSlots; ++i)
    {
        const int x = pageOrderArea.getX() + i * (slotWidth + gapX);
        const int y = pageOrderArea.getY();
        g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x),
                                                      static_cast<float>(y),
                                                      static_cast<float>(slotWidth),
                                                      static_cast<float>(slotHeight)),
                               5.0f);
    }

}

void MonomePagesPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(4);

    auto pageOrderArea = bounds.removeFromTop(58);
    const int numSlots = MlrVSTAudioProcessor::NumControlRowPages;
    const int gapX = 4;
    const int slotWidth = juce::jmax(52, (pageOrderArea.getWidth() - ((numSlots - 1) * gapX)) / juce::jmax(1, numSlots));
    const int slotHeight = pageOrderArea.getHeight();

    for (int i = 0; i < MlrVSTAudioProcessor::NumControlRowPages; ++i)
    {
        auto& row = rows[static_cast<size_t>(i)];
        juce::Rectangle<int> slotBounds(pageOrderArea.getX() + i * (slotWidth + gapX),
                                        pageOrderArea.getY(),
                                        slotWidth, slotHeight);

        auto header = slotBounds.removeFromTop(11);
        row.positionLabel.setBounds(header.removeFromLeft(18));
        slotBounds.removeFromTop(1);

        auto arrows = slotBounds.removeFromRight(16);
        row.modeButton.setBounds(slotBounds.reduced(0, 2));

        const int arrowW = 13;
        const int arrowH = 9;
        row.upButton.setBounds(arrows.getCentreX() - (arrowW / 2), arrows.getY() + 1, arrowW, arrowH);
        row.downButton.setBounds(arrows.getCentreX() - (arrowW / 2), arrows.getBottom() - arrowH - 1, arrowW, arrowH);
    }

}

void MonomePagesPanel::timerCallback()
{
    refreshFromProcessor();
}

void MonomePagesPanel::refreshFromProcessor()
{
    const auto order = processor.getControlPageOrder();
    const auto activeMode = processor.getCurrentControlMode();

    for (int i = 0; i < MlrVSTAudioProcessor::NumControlRowPages; ++i)
    {
        auto& row = rows[static_cast<size_t>(i)];
        const auto modeAtButton = order[static_cast<size_t>(i)];
        const bool isActive = (activeMode == modeAtButton) && (activeMode != MlrVSTAudioProcessor::ControlMode::Normal);

        row.positionLabel.setText("#" + juce::String(i + 1), juce::dontSendNotification);
        row.modeButton.setButtonText(getMonomePageShortName(modeAtButton));
        row.modeButton.setTooltip(getMonomePageDisplayName(modeAtButton));
        row.positionLabel.setColour(juce::Label::textColourId, isActive ? kAccent.brighter(0.15f) : kTextSecondary);
        row.modeButton.setColour(juce::TextButton::buttonColourId,
                                 isActive ? kAccent.withAlpha(0.78f) : juce::Colour(0xff3a3a3a));
        row.modeButton.setColour(juce::TextButton::textColourOffId,
                                 isActive ? juce::Colour(0xff111111) : juce::Colour(0xfff3f3f3));
        row.upButton.setEnabled(i > 0);
        row.downButton.setEnabled(i < (MlrVSTAudioProcessor::NumControlRowPages - 1));
        row.upButton.setColour(juce::TextButton::buttonColourId, isActive ? kAccent.withAlpha(0.6f) : juce::Colour(0xff454545));
        row.downButton.setColour(juce::TextButton::buttonColourId, isActive ? kAccent.withAlpha(0.6f) : juce::Colour(0xff454545));
    }

}

void MonomePagesPanel::updatePresetButtons()
{
    const int loadedPreset = processor.getLoadedPresetIndex();
    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        const bool exists = processor.presetExists(i);
        auto& button = presetButtons[static_cast<size_t>(i)];
        if (i == loadedPreset && exists)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffb8d478));
            button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff111111));
        }
        else
        {
            button.setColour(juce::TextButton::buttonColourId,
                             exists ? kAccent.withMultipliedBrightness(0.9f) : juce::Colour(0xff2b2b2b));
            button.setColour(juce::TextButton::textColourOffId,
                             exists ? juce::Colour(0xff111111) : kTextMuted);
        }
    }
}

void MonomePagesPanel::layoutPresetButtons()
{
    const int gap = 4;
    const int buttonHeight = 16;
    const int minButtonWidth = 26;

    const int viewportWidth = juce::jmax(0, presetViewport.getWidth() - presetViewport.getScrollBarThickness());
    const int buttonWidth = juce::jmax(minButtonWidth,
                                       (viewportWidth - ((MlrVSTAudioProcessor::PresetColumns - 1) * gap))
                                       / MlrVSTAudioProcessor::PresetColumns);
    const int contentWidth = (MlrVSTAudioProcessor::PresetColumns * buttonWidth)
                             + ((MlrVSTAudioProcessor::PresetColumns - 1) * gap);
    const int contentHeight = (MlrVSTAudioProcessor::PresetRows * buttonHeight)
                              + ((MlrVSTAudioProcessor::PresetRows - 1) * gap);

    presetGridContent.setSize(contentWidth, contentHeight);

    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        const int x = i % MlrVSTAudioProcessor::PresetColumns;
        const int y = i / MlrVSTAudioProcessor::PresetColumns;
        presetButtons[static_cast<size_t>(i)].setBounds(x * (buttonWidth + gap),
                                                        y * (buttonHeight + gap),
                                                        buttonWidth,
                                                        buttonHeight);
    }
}

void MonomePagesPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int deltaY = static_cast<int>(-wheel.deltaY * 96.0f);
    if (deltaY != 0)
        presetViewport.setViewPosition(presetViewport.getViewPositionX(),
                                       juce::jmax(0, presetViewport.getViewPositionY() + deltaY));
}

void MonomePagesPanel::onPresetButtonClicked(int presetIndex)
{
    if (juce::ModifierKeys::getCurrentModifiers().isShiftDown())
        processor.savePreset(presetIndex);
    else
        processor.loadPreset(presetIndex);

    updatePresetButtons();
}


//==============================================================================
// PatternControlPanel Implementation
//==============================================================================

PatternControlPanel::PatternControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    titleLabel.setText("Pattern Recorder", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    instructionsLabel.setVisible(false);

    timingLabel.setText("Beat: --", juce::dontSendNotification);
    timingLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    timingLabel.setColour(juce::Label::textColourId, kTextSecondary);
    timingLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(timingLabel);

    quantizeLabel.setText("Quantize: --", juce::dontSendNotification);
    quantizeLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    quantizeLabel.setColour(juce::Label::textColourId, kTextSecondary);
    quantizeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(quantizeLabel);
    
    // Pattern controls
    for (int i = 0; i < 4; ++i)
    {
        auto& pattern = patterns[i];

        pattern.nameLabel.setText("PATTERN " + juce::String(i + 1), juce::dontSendNotification);
        pattern.nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        pattern.nameLabel.setColour(juce::Label::textColourId, getStripColor(i));
        pattern.nameLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(pattern.nameLabel);
        
        pattern.recordButton.setButtonText("Rec");
        pattern.recordButton.setToggleable(true);
        pattern.recordButton.setTooltip("Record pattern events.");
        pattern.recordButton.onClick = [this, i]()
        {
            if (patterns[i].recordButton.getToggleState())
            {
                processor.getAudioEngine()->startPatternRecording(i);
            }
            else
            {
                processor.getAudioEngine()->stopPatternRecording(i);
            }
        };
        addAndMakeVisible(pattern.recordButton);
        styleUiButton(pattern.recordButton, true);
        
        pattern.playButton.setButtonText("Play");
        pattern.playButton.setToggleable(true);
        pattern.playButton.setTooltip("Play/loop this pattern.");
        pattern.playButton.onClick = [this, i]()
        {
            if (patterns[i].playButton.getToggleState())
                processor.getAudioEngine()->startPatternPlayback(i);
            else
                processor.getAudioEngine()->stopPatternPlayback(i);
        };
        addAndMakeVisible(pattern.playButton);
        styleUiButton(pattern.playButton);
        
        pattern.stopButton.setButtonText("Stop");
        pattern.stopButton.setTooltip("Stop pattern playback.");
        pattern.stopButton.onClick = [this, i]()
        {
            processor.getAudioEngine()->stopPatternPlayback(i);
            patterns[i].playButton.setToggleState(false, juce::dontSendNotification);
        };
        addAndMakeVisible(pattern.stopButton);
        styleUiButton(pattern.stopButton);
        
        pattern.clearButton.setButtonText("Clear");
        pattern.clearButton.setTooltip("Erase all events in this pattern.");
        pattern.clearButton.onClick = [this, i]()
        {
            processor.getAudioEngine()->clearPattern(i);
        };
        addAndMakeVisible(pattern.clearButton);
        styleUiButton(pattern.clearButton);
        
        pattern.statusLabel.setText("EMPTY", juce::dontSendNotification);
        pattern.statusLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        pattern.statusLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(pattern.statusLabel);

        pattern.detailLabel.setText("No events recorded", juce::dontSendNotification);
        pattern.detailLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
        pattern.detailLabel.setColour(juce::Label::textColourId, kTextSecondary);
        pattern.detailLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(pattern.detailLabel);
    }
    
    startTimer(100);
}

void PatternControlPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    juce::ColourGradient bg(juce::Colour(0xff2e2e2e), 0.0f, 0.0f,
                            juce::Colour(0xff242424), 0.0f, bounds.getBottom(), false);
    g.setGradientFill(bg);
    g.fillAll();

    auto content = getLocalBounds().reduced(10);
    content.removeFromTop(72);

    for (int i = 0; i < 4; ++i)
    {
        const auto rowHeight = 58;
        auto card = content.removeFromTop(rowHeight).toFloat();
        content.removeFromTop(6);

        g.setColour(juce::Colour(0xff2b2b2b));
        g.fillRoundedRectangle(card, 8.0f);

        g.setColour(kPanelStroke);
        g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 1.0f);

        g.setColour(getStripColor(i).withAlpha(0.85f));
        g.fillRoundedRectangle(card.removeFromLeft(3.0f), 2.0f);
    }
}

void PatternControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    auto topRow = bounds.removeFromTop(24);
    titleLabel.setBounds(topRow.removeFromLeft(bounds.getWidth() / 2));
    timingLabel.setBounds(topRow.removeFromRight(130));
    quantizeLabel.setBounds(topRow.removeFromRight(140));

    bounds.removeFromTop(8);

    const int rowHeight = 60;
    const int rowGap = 6;

    for (int i = 0; i < 4; ++i)
    {
        auto patternBounds = bounds.removeFromTop(rowHeight).reduced(10, 8);
        bounds.removeFromTop(rowGap);
        
        auto& pattern = patterns[i];

        auto header = patternBounds.removeFromTop(18);
        pattern.nameLabel.setBounds(header.removeFromLeft(130));
        pattern.statusLabel.setBounds(header.removeFromRight(120));

        auto controls = patternBounds.removeFromTop(26);
        pattern.recordButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.playButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.stopButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.clearButton.setBounds(controls.removeFromLeft(64));

        pattern.detailLabel.setBounds(patternBounds);
    }
}

void PatternControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updatePatternStates();
}

void PatternControlPanel::updatePatternStates()
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    const double beat = engine->getCurrentBeat();
    timingLabel.setText("Beat: " + juce::String(beat, 2), juce::dontSendNotification);

    if (auto* quantizeParam = processor.parameters.getRawParameterValue("quantize"))
    {
        static const char* values[] = {"1", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
        int idx = juce::jlimit(0, 9, static_cast<int>(*quantizeParam));
        quantizeLabel.setText("Quantize: " + juce::String(values[idx]), juce::dontSendNotification);
    }

    for (int i = 0; i < 4; ++i)
    {
        auto* pattern = engine->getPattern(i);
        if (pattern)
        {
            patterns[i].recordButton.setToggleState(pattern->isRecording(), juce::dontSendNotification);
            patterns[i].playButton.setToggleState(pattern->isPlaying(), juce::dontSendNotification);

            const int eventCount = pattern->getEventCount();
            const int lengthBeats = pattern->getLengthInBeats();
            const double startBeat = pattern->getRecordingStartBeat();

            if (pattern->isRecording())
            {
                const double beatsLeft = juce::jmax(0.0, (startBeat + static_cast<double>(lengthBeats)) - beat);
                patterns[i].statusLabel.setText("RECORDING", juce::dontSendNotification);
                patterns[i].statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd46b62));
                patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • Ends in " + juce::String(beatsLeft, 2) + " beats",
                                                juce::dontSendNotification);
            }
            else if (pattern->isPlaying())
            {
                patterns[i].statusLabel.setText("PLAYING", juce::dontSendNotification);
                patterns[i].statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff76be7e));
                patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • " + juce::String(eventCount) + " events",
                                                juce::dontSendNotification);
            }
            else
            {
                if (eventCount > 0)
                {
                    patterns[i].statusLabel.setText("READY", juce::dontSendNotification);
                    patterns[i].statusLabel.setColour(juce::Label::textColourId, kAccent.withMultipliedBrightness(1.1f));
                    patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • " + juce::String(eventCount) + " events",
                                                    juce::dontSendNotification);
                }
                else
                {
                    patterns[i].statusLabel.setText("EMPTY", juce::dontSendNotification);
                    patterns[i].statusLabel.setColour(juce::Label::textColourId, kTextMuted);
                    patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • No events recorded",
                                                    juce::dontSendNotification);
                }
            }
        }
    }
}

//==============================================================================
// GroupControlPanel Implementation
//==============================================================================

GroupControlPanel::GroupControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    // Title
    titleLabel.setText("Mute Groups", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(titleLabel);
    
    // Group controls
    for (int i = 0; i < 4; ++i)
    {
        auto& group = groups[i];
        
        group.nameLabel.setText("Group " + juce::String(i + 1), juce::dontSendNotification);
        group.nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        addAndMakeVisible(group.nameLabel);
        
        group.muteButton.setButtonText("Mute");
        group.muteButton.setToggleable(true);
        group.muteButton.setTooltip("Mute/unmute this group.");
        group.muteButton.onClick = [this, i]()
        {
            if (auto* grp = processor.getAudioEngine()->getGroup(i))
            {
                grp->setMuted(groups[i].muteButton.getToggleState());
            }
        };
        addAndMakeVisible(group.muteButton);
        styleUiButton(group.muteButton, true);
        
        group.volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        group.volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        group.volumeSlider.setRange(0.0, 1.0, 0.01);
        group.volumeSlider.setValue(1.0);
        enableAltClickReset(group.volumeSlider, 1.0);
        group.volumeSlider.onValueChange = [this, i]()
        {
            if (auto* grp = processor.getAudioEngine()->getGroup(i))
            {
                grp->setVolume(static_cast<float>(groups[i].volumeSlider.getValue()));
            }
        };
        addAndMakeVisible(group.volumeSlider);
        
        group.statusLabel.setText("No strips", juce::dontSendNotification);
        group.statusLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
        group.statusLabel.setColour(juce::Label::textColourId, kTextMuted);
        addAndMakeVisible(group.statusLabel);
    }
    
    startTimer(200);
}

void GroupControlPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    juce::ColourGradient pageFill(kPanelTop.darker(0.35f), 0.0f, 0.0f,
                                  kPanelBottom.darker(0.4f), 0.0f, bounds.getBottom(), false);
    g.setGradientFill(pageFill);
    g.fillAll();
    
    // Rainbow colors for 4 groups
    const juce::Colour rainbowColors[] = {
        juce::Colour(0xff5ea5a8),
        juce::Colour(0xff6f93c8),
        juce::Colour(0xffd36f63),
        juce::Colour(0xffd18f4f)
    };
    
    // Draw rounded rectangles with rainbow dividers for each group
    float groupHeight = (bounds.getHeight() - 60.0f) / 4.0f;  // Account for title
    float startY = 40.0f;
    
    for (int i = 0; i < 4; ++i)
    {
        auto groupBounds = juce::Rectangle<float>(
            bounds.getX() + 4.0f,
            startY + (i * groupHeight),
            bounds.getWidth() - 8.0f,
            groupHeight - 4.0f
        );
        
        // Rounded background
        g.setColour(juce::Colour(0xff2b2b2b));
        g.fillRoundedRectangle(groupBounds, 8.0f);
        
        // Rainbow divider at bottom
        g.setColour(rainbowColors[i]);
        auto dividerRect = juce::Rectangle<float>(
            groupBounds.getX() + 8.0f,
            groupBounds.getBottom() - 6.0f,
            groupBounds.getWidth() - 16.0f,
            2.0f
        );
        g.fillRoundedRectangle(dividerRect, 1.0f);
    }
}

void GroupControlPanel::resized()
{
    auto bounds = getLocalBounds();
    
    // Title at top
    auto titleRow = bounds.removeFromTop(32);
    titleLabel.setBounds(titleRow.reduced(12, 6));
    
    bounds.removeFromTop(8);
    
    // Calculate group height
    float groupHeight = bounds.getHeight() / 4.0f;
    
    // Group rows - inside rounded rectangles
    for (int i = 0; i < 4; ++i)
    {
        auto groupBounds = bounds.removeFromTop(static_cast<int>(groupHeight));
        groupBounds.reduce(12, 8);  // Padding inside rounded rect
        
        auto& group = groups[i];
        
        // Name row
        auto nameRow = groupBounds.removeFromTop(22);
        group.nameLabel.setBounds(nameRow);
        
        groupBounds.removeFromTop(4);
        
        // Control row
        auto controlRow = groupBounds.removeFromTop(28);
        group.muteButton.setBounds(controlRow.removeFromLeft(82));
        controlRow.removeFromLeft(6);
        group.volumeSlider.setBounds(controlRow.removeFromLeft(140));
        controlRow.removeFromLeft(10);
        group.statusLabel.setBounds(controlRow);
    }
}

void GroupControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateGroupStates();
}

void GroupControlPanel::updateGroupStates()
{
    for (int i = 0; i < 4; ++i)
    {
        auto* group = processor.getAudioEngine()->getGroup(i);
        if (group)
        {
            auto strips = group->getStrips();
            if (strips.empty())
            {
                groups[i].statusLabel.setText("No strips", juce::dontSendNotification);
            }
            else
            {
                juce::String stripList;
                for (size_t j = 0; j < strips.size(); ++j)
                {
                    if (j > 0) stripList += ", ";
                    stripList += juce::String(strips[j] + 1);
                }
                groups[i].statusLabel.setText("Strips: " + stripList, juce::dontSendNotification);
            }
            
            groups[i].muteButton.setToggleState(group->isMuted(), juce::dontSendNotification);
        }
    }
}

//==============================================================================
// ModulationControlPanel Implementation
//==============================================================================

ModulationControlPanel::ModulationControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    titleLabel.setText("Per-Row Modulation Sequencer", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    addAndMakeVisible(titleLabel);

    stripLabel.setColour(juce::Label::textColourId, kAccent);
    stripLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    addAndMakeVisible(stripLabel);

    targetLabel.setText("Target", juce::dontSendNotification);
    targetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(targetLabel);

    targetBox.addItem("None", 1);
    targetBox.addItem("Volume", 2);
    targetBox.addItem("Pan", 3);
    targetBox.addItem("Pitch", 4);
    targetBox.addItem("Speed", 5);
    targetBox.addItem("Cutoff", 6);
    targetBox.addItem("Resonance", 7);
    targetBox.addItem("Grain Size", 8);
    targetBox.addItem("Grain Density", 9);
    targetBox.addItem("Grain Pitch", 10);
    targetBox.addItem("Grain Pitch Jitter", 11);
    targetBox.addItem("Grain Spread", 12);
    targetBox.addItem("Grain Jitter", 13);
    targetBox.addItem("Grain Random", 14);
    targetBox.addItem("Grain Arp", 15);
    targetBox.addItem("Grain Cloud", 16);
    targetBox.addItem("Grain Emitter", 17);
    targetBox.addItem("Grain Envelope", 18);
    targetBox.addItem("Retrigger", 19);
    targetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            engine->setModTarget(selectedStrip, comboIdToModTarget(targetBox.getSelectedId()));
            bipolarToggle.setToggleState(engine->isModBipolar(selectedStrip), juce::dontSendNotification);
        }
    };
    addAndMakeVisible(targetBox);

    bipolarToggle.setButtonText("Bipolar");
    bipolarToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModBipolar(selectedStrip, bipolarToggle.getToggleState());
    };
    addAndMakeVisible(bipolarToggle);

    depthLabel.setText("Depth", juce::dontSendNotification);
    depthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(depthLabel);

    depthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    depthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 34, 16);
    depthSlider.setRange(0.0, 1.0, 0.01);
    depthSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModDepth(selectedStrip, static_cast<float>(depthSlider.getValue()));
    };
    addAndMakeVisible(depthSlider);

    offsetLabel.setVisible(false);
    offsetSlider.setVisible(false);

    lengthLabel.setText("Length", juce::dontSendNotification);
    lengthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(lengthLabel);

    lengthBox.addItem("1", 1);
    lengthBox.addItem("2", 2);
    lengthBox.addItem("4", 4);
    lengthBox.addItem("8", 8);
    lengthBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            const int bars = lengthBox.getSelectedId();
            const int currentPage = engine->getModEditPage(selectedStrip);
            engine->setModLengthBars(selectedStrip, bars);
            engine->setModEditPage(selectedStrip, juce::jlimit(0, juce::jmax(0, bars - 1), currentPage));
        }
    };
    addAndMakeVisible(lengthBox);

    pageLabel.setText("Page", juce::dontSendNotification);
    pageLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(pageLabel);

    pageBox.addItem("1", 1);
    pageBox.addItem("2", 2);
    pageBox.addItem("3", 3);
    pageBox.addItem("4", 4);
    pageBox.addItem("5", 5);
    pageBox.addItem("6", 6);
    pageBox.addItem("7", 7);
    pageBox.addItem("8", 8);
    pageBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModEditPage(selectedStrip, juce::jlimit(0, 7, pageBox.getSelectedId() - 1));
    };
    addAndMakeVisible(pageBox);

    smoothLabel.setText("Smooth", juce::dontSendNotification);
    smoothLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(smoothLabel);

    smoothSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    smoothSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 34, 16);
    smoothSlider.setRange(0.0, 250.0, 1.0);
    smoothSlider.setSkewFactorFromMidPoint(40.0);
    smoothSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModSmoothingMs(selectedStrip, static_cast<float>(smoothSlider.getValue()));
    };
    addAndMakeVisible(smoothSlider);

    pitchScaleToggle.setButtonText("Pitch Quantize");
    pitchScaleToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScaleQuantize(selectedStrip, pitchScaleToggle.getToggleState());
    };
    addAndMakeVisible(pitchScaleToggle);

    pitchScaleLabel.setText("Scale", juce::dontSendNotification);
    pitchScaleLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(pitchScaleLabel);

    pitchScaleBox.addItem("Chromatic", 1);
    pitchScaleBox.addItem("Major", 2);
    pitchScaleBox.addItem("Minor", 3);
    pitchScaleBox.addItem("Dorian", 4);
    pitchScaleBox.addItem("Pentatonic", 5);
    pitchScaleBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScale(selectedStrip, comboIdToPitchScale(pitchScaleBox.getSelectedId()));
    };
    addAndMakeVisible(pitchScaleBox);

    gestureHintLabel.setText("Cell mods (same as Step): Cmd=Divide  Ctrl=Ramp+  Opt=Ramp-", juce::dontSendNotification);
    gestureHintLabel.setColour(juce::Label::textColourId, kTextMuted);
    gestureHintLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    gestureHintLabel.setJustificationType(juce::Justification::centredLeft);
    gestureHintLabel.setTooltip("Modifier drags on mod cells mirror step-sequencer cells.");
    addAndMakeVisible(gestureHintLabel);

    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        b.setButtonText(juce::String(i + 1));
        b.setTooltip("Click: toggle step. Cmd+drag: divide. Ctrl+drag: ramp up. Opt+drag: ramp down.");
        b.onClick = [this, i]()
        {
            if (suppressNextStepClick)
            {
                suppressNextStepClick = false;
                return;
            }
            if (getStepCellModifierGesture(juce::ModifierKeys::getCurrentModifiersRealtime()) != StepCellModifierGesture::None)
                return;
            if (auto* engine = processor.getAudioEngine())
                engine->toggleModStep(selectedStrip, i);
            refreshFromEngine();
        };
        b.addMouseListener(this, true);
        addAndMakeVisible(b);
    }

    startTimer(80);
    refreshFromEngine();
}

void ModulationControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void ModulationControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);
    titleLabel.setBounds(bounds.removeFromTop(22));
    stripLabel.setBounds(bounds.removeFromTop(18));
    bounds.removeFromTop(4);

    auto top = bounds.removeFromTop(22);
    targetLabel.setBounds(top.removeFromLeft(40));
    targetBox.setBounds(top.removeFromLeft(98));
    top.removeFromLeft(4);
    lengthLabel.setBounds(top.removeFromLeft(38));
    lengthBox.setBounds(top.removeFromLeft(56));
    top.removeFromLeft(4);
    pageLabel.setBounds(top.removeFromLeft(28));
    pageBox.setBounds(top.removeFromLeft(46));

    bounds.removeFromTop(3);
    auto depthRow = bounds.removeFromTop(22);
    depthLabel.setBounds(depthRow.removeFromLeft(44));
    depthSlider.setBounds(depthRow.removeFromLeft(120));
    depthRow.removeFromLeft(4);
    bipolarToggle.setBounds(depthRow.removeFromLeft(70));
    depthRow.removeFromLeft(4);
    pitchScaleToggle.setBounds(depthRow);

    bounds.removeFromTop(3);
    auto smoothRow = bounds.removeFromTop(22);
    smoothLabel.setBounds(smoothRow.removeFromLeft(44));
    smoothSlider.setBounds(smoothRow.removeFromLeft(120));

    bounds.removeFromTop(3);
    auto scaleRow = bounds.removeFromTop(22);
    pitchScaleLabel.setBounds(scaleRow.removeFromLeft(44));
    pitchScaleBox.setBounds(scaleRow.removeFromLeft(112));
    scaleRow.removeFromLeft(4);

    bounds.removeFromTop(4);
    gestureHintLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(6);
    const int gap = 4;
    const int w = juce::jmax(20, (bounds.getWidth() - (gap * (ModernAudioEngine::ModSteps - 1))) / ModernAudioEngine::ModSteps);
    const int h = juce::jmax(24, bounds.getHeight());
    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
        stepButtons[static_cast<size_t>(i)].setBounds(bounds.getX() + i * (w + gap), bounds.getY(), w, h);
}

void ModulationControlPanel::timerCallback()
{
    refreshFromEngine();
}

void ModulationControlPanel::mouseDown(const juce::MouseEvent& e)
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    const int step = stepIndexForComponent(e.eventComponent);
    if (step < 0)
        return;

    const auto modifierGesture = getStepCellModifierGesture(e.mods);
    if (modifierGesture != StepCellModifierGesture::None)
    {
        const auto state = engine->getModSequencerState(selectedStrip);
        gestureSourceValue = juce::jlimit(0.0f, 1.0f, state.steps[static_cast<size_t>(step)]);
        gestureSourceSubdivision = juce::jlimit(
            1,
            ModernAudioEngine::ModMaxStepSubdivisions,
            state.stepSubdivisions[static_cast<size_t>(step)]);
        gestureSourceEndValue = juce::jlimit(0.0f, 1.0f, state.stepEndValues[static_cast<size_t>(step)]);
        switch (modifierGesture)
        {
            case StepCellModifierGesture::Divide:
                gestureMode = EditGestureMode::DuplicateCell;
                break;
            case StepCellModifierGesture::RampUp:
                gestureMode = EditGestureMode::ShapeUpCell;
                break;
            case StepCellModifierGesture::RampDown:
                gestureMode = EditGestureMode::ShapeDownCell;
                break;
            case StepCellModifierGesture::None:
            default:
                gestureMode = EditGestureMode::None;
                break;
        }
        gestureActive = true;
        gestureStartY = e.getScreenPosition().y;
        gestureStep = step;
        suppressNextStepClick = true;

        if (gestureMode == EditGestureMode::ShapeUpCell)
            applyShapeGesture(0, true);
        else if (gestureMode == EditGestureMode::ShapeDownCell)
            applyShapeGesture(0, false);

        refreshFromEngine();
    }
}

void ModulationControlPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (!gestureActive || !processor.getAudioEngine())
        return;

    const int deltaY = e.getScreenPosition().y - gestureStartY;
    if (gestureMode == EditGestureMode::DuplicateCell)
        applyDuplicateGesture(deltaY);
    else if (gestureMode == EditGestureMode::ShapeUpCell)
        applyShapeGesture(deltaY, true);
    else if (gestureMode == EditGestureMode::ShapeDownCell)
        applyShapeGesture(deltaY, false);

    refreshFromEngine();
}

void ModulationControlPanel::mouseUp(const juce::MouseEvent&)
{
    gestureActive = false;
    gestureMode = EditGestureMode::None;
    gestureStep = -1;
}

int ModulationControlPanel::stepIndexForComponent(juce::Component* c) const
{
    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        if (c == &stepButtons[static_cast<size_t>(i)])
            return i;
    }
    return -1;
}

void ModulationControlPanel::applyDuplicateGesture(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || gestureStep < 0 || gestureStep >= ModernAudioEngine::ModSteps)
        return;

    const int nextSubdivision = juce::jlimit(
        1,
        ModernAudioEngine::ModMaxStepSubdivisions,
        gestureSourceSubdivision + ((-deltaY) / 14));
    const float endValue = (nextSubdivision > 1) ? gestureSourceEndValue : gestureSourceValue;
    engine->setModStepShape(selectedStrip, gestureStep, nextSubdivision, endValue);
}

void ModulationControlPanel::applyShapeGesture(int deltaY, bool rampUpMode)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || gestureStep < 0 || gestureStep >= ModernAudioEngine::ModSteps)
        return;

    int subdivisions = gestureSourceSubdivision;
    if (subdivisions <= 1)
    {
        subdivisions = juce::jlimit(
            2,
            ModernAudioEngine::ModMaxStepSubdivisions,
            2 + (std::abs(deltaY) / 14));
    }

    float startValue = gestureSourceValue;
    float endValue = gestureSourceEndValue;
    computeSingleModCellRamp(gestureSourceValue, gestureSourceEndValue, deltaY, rampUpMode, startValue, endValue);
    engine->setModStepValue(selectedStrip, gestureStep, startValue);
    engine->setModStepShape(selectedStrip, gestureStep, subdivisions, endValue);
}

void ModulationControlPanel::refreshFromEngine()
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    selectedStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, processor.getLastMonomePressedStripRow());
    stripLabel.setText("Selected Row: " + juce::String(selectedStrip + 1) + " (last pressed)", juce::dontSendNotification);

    const auto state = engine->getModSequencerState(selectedStrip);
    targetBox.setSelectedId(modTargetToComboId(state.target), juce::dontSendNotification);
    bipolarToggle.setToggleState(state.bipolar, juce::dontSendNotification);
    bipolarToggle.setEnabled(modTargetAllowsBipolar(state.target));
    depthSlider.setValue(state.depth, juce::dontSendNotification);
    lengthBox.setSelectedId(state.lengthBars, juce::dontSendNotification);
    pageBox.setSelectedId(juce::jlimit(1, 8, state.editPage + 1), juce::dontSendNotification);
    pageBox.setEnabled(state.lengthBars > 1);
    smoothSlider.setValue(state.smoothingMs, juce::dontSendNotification);
    pitchScaleToggle.setToggleState(state.pitchScaleQuantize, juce::dontSendNotification);
    pitchScaleBox.setSelectedId(pitchScaleToComboId(static_cast<ModernAudioEngine::PitchScale>(state.pitchScale)), juce::dontSendNotification);
    pitchScaleLabel.setEnabled(state.pitchScaleQuantize);
    pitchScaleBox.setEnabled(state.pitchScaleQuantize);

    const int activeGlobalStep = engine->getModCurrentGlobalStep(selectedStrip);
    const int playbackPage = juce::jlimit(
        0,
        ModernAudioEngine::MaxModBars - 1,
        activeGlobalStep / ModernAudioEngine::ModSteps);
    const int activeStep = (playbackPage == state.editPage)
        ? (activeGlobalStep % ModernAudioEngine::ModSteps)
        : -1;
    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        const float value = juce::jlimit(0.0f, 1.0f, state.steps[static_cast<size_t>(i)]);
        const int subdivisions = juce::jlimit(
            1,
            ModernAudioEngine::ModMaxStepSubdivisions,
            state.stepSubdivisions[static_cast<size_t>(i)]);
        const float endValue = juce::jlimit(0.0f, 1.0f, state.stepEndValues[static_cast<size_t>(i)]);
        const auto offColour = juce::Colour(0xff2f2f2f);
        const auto onColour = kAccent.withMultipliedBrightness(0.9f);
        juce::Colour c = offColour.interpolatedWith(onColour, value);
        if (subdivisions > 1)
            c = c.interpolatedWith(juce::Colour(0xfff0f6ff), 0.16f);
        if (i == activeStep)
            c = c.interpolatedWith(juce::Colour(0xffffcf75), 0.55f);
        b.setColour(juce::TextButton::buttonColourId, c);
        b.setTooltip("Step " + juce::String(i + 1)
                     + ": " + juce::String(static_cast<int>(std::round(value * 100.0f))) + "%\n"
                     + "Shape: x" + juce::String(subdivisions)
                     + "  end " + juce::String(static_cast<int>(std::round(endValue * 100.0f))) + "%\n"
                     + "Click: toggle step. Cmd+drag: divide. Ctrl+drag: ramp up. Opt+drag: ramp down.");
    }
}


//==============================================================================
// MlrVSTAudioProcessorEditor Implementation
//==============================================================================

MlrVSTAudioProcessorEditor::MlrVSTAudioProcessorEditor(MlrVSTAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setupLookAndFeel();
    setTooltipsEnabled(false);
    activeGuiStripCount = getDetectedGuiStripCount();
    
    // Enable keyboard input for spacebar transport control
    setWantsKeyboardFocus(true);
    
    // Set window size FIRST. Clamp initial height to the current display so hosts
    // that do not expose plugin resizing (e.g. Logic for some formats) still show
    // the full UI without clipping the bottom.
    int initialHeight = windowHeight;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const int safeVisibleHeight = juce::jmax(620, display->userArea.getHeight() - 80);
        initialHeight = juce::jmin(windowHeight, safeVisibleHeight);
    }
    setSize(windowWidth, initialHeight);
    setResizable(true, true);
    setResizeLimits(1000, 620, 1920, 1400);
    
    // Create all UI components
    createUIComponents();
    setActiveGuiStripCount(activeGuiStripCount, true);
    
    // Force initial layout
    resized();
    
    // Start UI update timer
    startTimer(50);
    lastPresetRefreshToken = audioProcessor.getPresetRefreshToken();
}

int MlrVSTAudioProcessorEditor::getDetectedGuiStripCount() const
{
    const int reportedStripCount = audioProcessor.getMonomeActiveStripCount();
    if (reportedStripCount <= 0)
        return 6;

    return juce::jlimit(1, MlrVSTAudioProcessor::MaxStrips, reportedStripCount);
}

void MlrVSTAudioProcessorEditor::setActiveGuiStripCount(int stripCount, bool forceRelayout)
{
    const int clampedStripCount = juce::jlimit(1, MlrVSTAudioProcessor::MaxStrips, stripCount);
    if (!forceRelayout && clampedStripCount == activeGuiStripCount)
        return;

    activeGuiStripCount = clampedStripCount;

    for (int i = 0; i < stripControls.size(); ++i)
        if (auto* strip = stripControls[i])
            strip->setVisible(i < activeGuiStripCount);

    for (int i = 0; i < fxStripControls.size(); ++i)
        if (auto* fxStrip = fxStripControls[i])
            fxStrip->setVisible(i < activeGuiStripCount);

    if (mainTabs)
    {
        if (auto* playPanel = mainTabs->getTabContentComponent(0))
            playPanel->resized();
        if (auto* fxPanel = mainTabs->getTabContentComponent(1))
            fxPanel->resized();
    }

    repaint();
}

void MlrVSTAudioProcessorEditor::createUIComponents()
{
    constexpr int kTotalSampleStrips = MlrVSTAudioProcessor::MaxStrips;
    // Monome grid hidden to save space - use physical monome instead
    monomeGrid = std::make_unique<MonomeGridDisplay>(audioProcessor);
    // Don't add to view - saves space
    
    // Create control panels
    monomeControl = std::make_unique<MonomeControlPanel>(audioProcessor);
    globalControl = std::make_unique<GlobalControlPanel>(audioProcessor);
    globalControl->onTooltipsToggled = [this](bool enabled)
    {
        setTooltipsEnabled(enabled);
    };
    monomePagesControl = std::make_unique<MonomePagesPanel>(audioProcessor);
    presetControl = std::make_unique<PresetControlPanel>(audioProcessor);
    pathsControl = std::make_unique<PathsControlPanel>(audioProcessor);
    
    // Create TABBED top controls to save space
    topTabs = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    topTabs->addTab("Global Controls", juce::Colour(0xff2c2c2c), globalControl.get(), false);
    topTabs->addTab("Presets", juce::Colour(0xff2c2c2c), presetControl.get(), false);
    topTabs->addTab("Monome Device", juce::Colour(0xff2c2c2c), monomeControl.get(), false);
    topTabs->addTab("Paths", juce::Colour(0xff2c2c2c), pathsControl.get(), false);
    topTabs->setTabBarDepth(28);
    topTabs->setCurrentTabIndex(0);  // Global Controls visible by default
    addAndMakeVisible(*topTabs);
    addAndMakeVisible(*monomePagesControl);
    
    // Helper panel classes for main tabs
    struct PlayPanel : public juce::Component
    {
        juce::OwnedArray<StripControl>& strips;
        int& visibleStripCount;
        
        PlayPanel(juce::OwnedArray<StripControl>& s, int& visibleCount)
            : strips(s), visibleStripCount(visibleCount) {}
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            const int gap = 1;
            const int totalStrips = strips.size();
            if (totalStrips <= 0)
                return;

            const int stripCount = juce::jlimit(1, totalStrips, visibleStripCount);
            const int totalGap = gap * juce::jmax(0, stripCount - 1);
            const int stripHeight = juce::jmax(1, (bounds.getHeight() - totalGap) / stripCount);
            
            for (int i = 0; i < stripCount; ++i)
            {
                if (auto* strip = strips[i])
                {
                    const int y = i * (stripHeight + gap);
                    strip->setBounds(0, y, bounds.getWidth(), stripHeight);
                }
            }
        }
    };
    
    struct FXPanel : public juce::Component
    {
        juce::OwnedArray<FXStripControl>& strips;
        int& visibleStripCount;
        
        FXPanel(juce::OwnedArray<FXStripControl>& s, int& visibleCount)
            : strips(s), visibleStripCount(visibleCount) {}
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            const int gap = 1;
            const int totalStrips = strips.size();
            if (totalStrips <= 0)
                return;

            const int stripCount = juce::jlimit(1, totalStrips, visibleStripCount);
            const int totalGap = gap * juce::jmax(0, stripCount - 1);
            const int stripHeight = juce::jmax(1, (bounds.getHeight() - totalGap) / stripCount);
            
            for (int i = 0; i < stripCount; ++i)
            {
                if (auto* strip = strips[i])
                {
                    const int y = i * (stripHeight + gap);
                    strip->setBounds(0, y, bounds.getWidth(), stripHeight);
                }
            }
        }
    };
    
    // Create MAIN UNIFIED TABS: Play / FX / Patterns / Groups
    mainTabs = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    
    // PLAY TAB - regular strip controls
    auto* playPanel = new PlayPanel(stripControls, activeGuiStripCount);
    for (int i = 0; i < kTotalSampleStrips; ++i)
    {
        auto* strip = new StripControl(i, audioProcessor);
        stripControls.add(strip);
        playPanel->addAndMakeVisible(strip);
    }
    
    // FX TAB - filter controls for each strip
    auto* fxPanel = new FXPanel(fxStripControls, activeGuiStripCount);
    for (int i = 0; i < kTotalSampleStrips; ++i)
    {
        auto* fxStrip = new FXStripControl(i, audioProcessor);
        fxStripControls.add(fxStrip);
        fxPanel->addAndMakeVisible(fxStrip);
    }
    
    // PATTERNS TAB
    patternControl = std::make_unique<PatternControlPanel>(audioProcessor);
    
    // GROUPS TAB
    groupControl = std::make_unique<GroupControlPanel>(audioProcessor);

    // Add main tabs to container
    mainTabs->addTab("Play", juce::Colour(0xff282828), playPanel, true);
    mainTabs->addTab("FX", juce::Colour(0xff282828), fxPanel, true);
    mainTabs->addTab("Patterns", juce::Colour(0xff282828), patternControl.get(), false);
    mainTabs->addTab("Groups", juce::Colour(0xff282828), groupControl.get(), false);
    mainTabs->setTabBarDepth(28);
    mainTabs->setCurrentTabIndex(0);  // Start on Play tab
    addAndMakeVisible(*mainTabs);
}

MlrVSTAudioProcessorEditor::~MlrVSTAudioProcessorEditor()
{
    stopTimer();
}

void MlrVSTAudioProcessorEditor::setupLookAndFeel()
{
    darkLookAndFeel.setDefaultSansSerifTypefaceName("Helvetica Neue");

    darkLookAndFeel.setColour(juce::ResizableWindow::backgroundColourId, kBgBottom);

    darkLookAndFeel.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff404448));
    darkLookAndFeel.setColour(juce::TextButton::buttonOnColourId, kAccent);
    darkLookAndFeel.setColour(juce::TextButton::textColourOffId, kTextPrimary);
    darkLookAndFeel.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff151515));

    darkLookAndFeel.setColour(juce::Slider::thumbColourId, kAccent);
    darkLookAndFeel.setColour(juce::Slider::trackColourId, juce::Colour(0xff4c4c4c));
    darkLookAndFeel.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff262626));
    darkLookAndFeel.setColour(juce::Slider::rotarySliderFillColourId, kAccent.withAlpha(0.9f));
    darkLookAndFeel.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff525252));

    darkLookAndFeel.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff32363a));
    darkLookAndFeel.setColour(juce::ComboBox::textColourId, kTextPrimary);
    darkLookAndFeel.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff545454));
    darkLookAndFeel.setColour(juce::ComboBox::arrowColourId, kTextSecondary);
    darkLookAndFeel.setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff262626));
    darkLookAndFeel.setColour(juce::PopupMenu::textColourId, kTextPrimary);
    darkLookAndFeel.setColour(juce::PopupMenu::highlightedBackgroundColourId, kAccent.withAlpha(0.35f));
    darkLookAndFeel.setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(0xfff7f7f7));

    darkLookAndFeel.setColour(juce::Label::textColourId, kTextPrimary);

    darkLookAndFeel.setColour(juce::TabbedComponent::backgroundColourId, juce::Colour(0xff23262a));
    darkLookAndFeel.setColour(juce::TabbedComponent::outlineColourId, juce::Colour(0xff575c61));
    darkLookAndFeel.setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colour(0xff575c61));
    darkLookAndFeel.setColour(juce::TabbedButtonBar::tabTextColourId, kTextSecondary);
    darkLookAndFeel.setColour(juce::TabbedButtonBar::frontTextColourId, juce::Colour(0xfff7f7f7));
    
    setLookAndFeel(&darkLookAndFeel);
}

void MlrVSTAudioProcessorEditor::setTooltipsEnabled(bool enabled)
{
    tooltipsEnabled = enabled;
    if (tooltipsEnabled)
    {
        if (!tooltipWindow)
            tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 350);
    }
    else
    {
        tooltipWindow.reset();
    }
}

void MlrVSTAudioProcessorEditor::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    juce::ColourGradient bg(kBgTop, 0.0f, 0.0f, kBgBottom, 0.0f, area.getBottom(), false);
    g.setGradientFill(bg);
    g.fillAll();

    auto titleBar = getLocalBounds().removeFromTop(40).toFloat();
    juce::ColourGradient titleFill(juce::Colour(0xff3a3d41), 0.0f, titleBar.getY(),
                                   juce::Colour(0xff2e3135), 0.0f, titleBar.getBottom(), false);
    g.setGradientFill(titleFill);
    g.fillRect(titleBar);
    g.setColour(juce::Colour(0xff565656));
    g.drawLine(titleBar.getX(), titleBar.getBottom(), titleBar.getRight(), titleBar.getBottom(), 1.0f);

    g.setColour(kTextPrimary);
    g.setFont(juce::Font(juce::FontOptions(23.0f, juce::Font::bold)));
    g.drawText("mlrVST", 16, 7, 220, 30, juce::Justification::centredLeft);

    g.setColour(kTextSecondary.brighter(0.1f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("Performance Slicer", 152, 10, 170, 20, juce::Justification::centredLeft);

    g.setColour(kTextMuted);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    const juce::String buildInfo = "v" + juce::String(JucePlugin_VersionString)
        + " | build " + juce::String(__DATE__) + " " + juce::String(__TIME__);
    g.drawText(buildInfo, getWidth() - 440, 11, 424, 18, juce::Justification::centredRight);
}

bool MlrVSTAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    (void) key;
    // Spacebar does nothing in plugin mode - DAW controls transport
    return false;  // Let other keys pass through
}

void MlrVSTAudioProcessorEditor::resized()
{
    // Safety check
    if (!topTabs || !mainTabs)
        return;
    
    auto bounds = getLocalBounds();
    
    // Title area
    bounds.removeFromTop(40);
    
    auto margin = 6;
    bounds.reduce(margin, margin);
    
    // Top section: TABBED controls (Global/Presets/Monome)
    auto topBar = bounds.removeFromTop(124);
    topTabs->setBounds(topBar);
    
    bounds.removeFromTop(margin);
    
    // MAIN AREA: Unified tabs (Play/FX/Patterns/Groups)
    auto monomePagesArea = bounds.removeFromBottom(50);
    monomePagesControl->setBounds(monomePagesArea);
    bounds.removeFromBottom(margin);
    mainTabs->setBounds(bounds);
}

//==============================================================================

void MlrVSTAudioProcessorEditor::timerCallback()
{
    if (!audioProcessor.getAudioEngine())
        return;
    
    // Update input meters
    if (globalControl)
    {
        float levelL = audioProcessor.getAudioEngine()->getInputLevelL();
        float levelR = audioProcessor.getAudioEngine()->getInputLevelR();
        globalControl->updateMeters(levelL, levelR);
        globalControl->refreshFromProcessor();
    }

    if (presetControl)
        presetControl->refreshVisualState();

    const bool modulationActive = audioProcessor.isControlModeActive()
        && audioProcessor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation;
    setActiveGuiStripCount(getDetectedGuiStripCount(), false);
    for (int i = 0; i < stripControls.size(); ++i)
    {
        if (auto* strip = stripControls[i])
        {
            const bool isVisibleStrip = i < activeGuiStripCount;
            const bool showLane = modulationActive && isVisibleStrip;
            strip->setModulationLaneView(showLane);
            strip->setVisible(isVisibleStrip);
        }
    }

    const uint32_t refreshToken = audioProcessor.getPresetRefreshToken();
    if (refreshToken != lastPresetRefreshToken)
    {
        lastPresetRefreshToken = refreshToken;
        if (patternControl)
            patternControl->timerCallback();
        if (groupControl)
            groupControl->timerCallback();
        for (auto* strip : stripControls)
            if (strip) strip->repaint();
        for (auto* fxStrip : fxStripControls)
            if (fxStrip) fxStrip->repaint();
        repaint();
    }
    
    // Update grid from monome connection
    if (auto& monome = audioProcessor.getMonomeConnection(); monome.isConnected())
    {
        if (monomeGrid)
            monomeGrid->updateFromEngine();
    }
}
