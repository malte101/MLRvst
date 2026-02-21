/*
  ==============================================================================

    PluginEditor.cpp
    Modern Comprehensive UI Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
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
        case 1:
        default: return ModernAudioEngine::ModTarget::None;
    }
}

bool modTargetAllowsBipolar(ModernAudioEngine::ModTarget target)
{
    return target == ModernAudioEngine::ModTarget::Pan
        || target == ModernAudioEngine::ModTarget::Pitch
        || target == ModernAudioEngine::ModTarget::Speed
        || target == ModernAudioEngine::ModTarget::GrainPitch;
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
        case ModernAudioEngine::ModCurveShape::Power: return 1;
        case ModernAudioEngine::ModCurveShape::SCurve: return 2;
        case ModernAudioEngine::ModCurveShape::Snap: return 3;
        case ModernAudioEngine::ModCurveShape::Stair: return 4;
        default: return 1;
    }
}

ModernAudioEngine::ModCurveShape comboIdToCurveShape(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::ModCurveShape::SCurve;
        case 3: return ModernAudioEngine::ModCurveShape::Snap;
        case 4: return ModernAudioEngine::ModCurveShape::Stair;
        case 1:
        default: return ModernAudioEngine::ModCurveShape::Power;
    }
}

float shapeCurvePhaseUi(float phase01, float bend, ModernAudioEngine::ModCurveShape shape)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);

    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::SCurve:
        {
            const float blend = juce::jmap(amount, 0.0f, 0.95f);
            float s = (t * t) * (3.0f - (2.0f * t));
            if (b >= 0.0f)
                s = std::pow(s, juce::jmap(amount, 1.0f, 5.5f));
            else
                s = 1.0f - std::pow(1.0f - s, juce::jmap(amount, 1.0f, 5.5f));
            return juce::jlimit(0.0f, 1.0f, juce::jmap(blend, t, s));
        }
        case ModernAudioEngine::ModCurveShape::Snap:
        {
            const float exp = juce::jmap(amount, 1.0f, 10.0f);
            return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
        }
        case ModernAudioEngine::ModCurveShape::Stair:
        {
            const int steps = juce::jlimit(3, 24, static_cast<int>(std::round(3.0f + (amount * 21.0f))));
            const float q = std::round(t * static_cast<float>(steps)) / static_cast<float>(steps);
            return b >= 0.0f ? q : (1.0f - q);
        }
        case ModernAudioEngine::ModCurveShape::Power:
        default:
        {
            const float exp = juce::jmap(amount, 1.0f, 7.0f);
            return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
        }
    }
}

struct GateRateEntry
{
    float cyclesPerBeat;
    const char* label;
};

constexpr std::array<GateRateEntry, 10> kGateRates {{
    { 0.5f, "1/2"  },
    { 0.75f, "1/2T" },
    { 1.0f, "1/4"  },
    { 1.5f, "1/4T" },
    { 2.0f, "1/8"  },
    { 3.0f, "1/8T" },
    { 4.0f, "1/16" },
    { 6.0f, "1/16T"},
    { 8.0f, "1/32" },
    { 0.25f, "1/1" }
}};

int gateRateIdFromCycles(float cyclesPerBeat)
{
    int best = 1;
    float bestDiff = std::abs(cyclesPerBeat - kGateRates[0].cyclesPerBeat);
    for (int i = 1; i < static_cast<int>(kGateRates.size()); ++i)
    {
        const float diff = std::abs(cyclesPerBeat - kGateRates[static_cast<size_t>(i)].cyclesPerBeat);
        if (diff < bestDiff)
        {
            best = i + 1;
            bestDiff = diff;
        }
    }
    return best;
}

float gateRateCyclesFromId(int id)
{
    const int idx = juce::jlimit(0, static_cast<int>(kGateRates.size()) - 1, id - 1);
    return kGateRates[static_cast<size_t>(idx)].cyclesPerBeat;
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
    if (hasAudio)
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
    stepDisplay.onStepClicked = [this](int stepIndex)
    {
        // Toggle step when clicked
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
            {
                strip->toggleStepAtIndex(stepIndex);
            }
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
            scratchSlider.setVisible(!isStepMode);
            scratchLabel.setVisible(!isStepMode);
            patternLengthBox.setVisible(isStepMode);
            patternLengthLabel.setVisible(isStepMode);
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
    
    speedSlider.setLookAndFeel(&knobLookAndFeel);
    speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    speedSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedSlider.setRange(0.0, 4.0, 0.01);
    speedSlider.setValue(1.0);
    enableAltClickReset(speedSlider, 1.0);
    speedSlider.setSkewFactorFromMidPoint(1.0);
    speedSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(speedSlider);
    
    speedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripSpeed" + juce::String(stripIndex), speedSlider);
    
    // Scratch slider - small, compact
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
    scratchSlider.onValueChange = [this]() {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setScratchAmount(static_cast<float>(scratchSlider.getValue()));
    };
    addAndMakeVisible(scratchSlider);

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
    setupGrainKnob(grainPitchJitterSlider, grainPitchJitterLabel, "PJIT", 0.0, 48.0, 0.1);
    setupGrainKnob(grainSpreadSlider, grainSpreadLabel, "SPRD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainJitterSlider, grainJitterLabel, "SJTR", 0.0, 1.0, 0.01);
    setupGrainKnob(grainRandomSlider, grainRandomLabel, "RAND", 0.0, 1.0, 0.01);
    setupGrainKnob(grainArpSlider, grainArpLabel, "ARP", 0.0, 1.0, 0.01);
    setupGrainKnob(grainCloudSlider, grainCloudLabel, "CLOUD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEmitterSlider, grainEmitterLabel, "EMIT", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEnvelopeSlider, grainEnvelopeLabel, "ENV", 0.0, 1.0, 0.01);
    enableAltClickReset(grainSizeSlider, 1240.0);
    enableAltClickReset(grainDensitySlider, 0.05);
    enableAltClickReset(grainPitchSlider, 0.0);
    enableAltClickReset(grainPitchJitterSlider, 0.0);
    enableAltClickReset(grainSpreadSlider, 0.0);
    enableAltClickReset(grainJitterSlider, 0.0);
    enableAltClickReset(grainRandomSlider, 0.0);
    enableAltClickReset(grainArpSlider, 0.0);
    enableAltClickReset(grainCloudSlider, 0.0);
    enableAltClickReset(grainEmitterSlider, 0.0);
    enableAltClickReset(grainEnvelopeSlider, 0.0);
    auto setupMini = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };
    setupMini(grainPitchSlider);
    setupMini(grainPitchJitterSlider);
    setupMini(grainSpreadSlider);
    setupMini(grainJitterSlider);
    setupMini(grainRandomSlider);
    setupMini(grainArpSlider);
    setupMini(grainCloudSlider);
    setupMini(grainEmitterSlider);
    setupMini(grainEnvelopeSlider);
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
        return juce::String(value, 1) + " ms (" + juce::String(sizeDivisionLabels[static_cast<size_t>(idx)]) + ")";
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
    grainRandomSlider.setTooltip("RAND: macro random depth (position, pitch, size, reverse), not just position jitter.");

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
                grainArpModeSlider.setValue(static_cast<double>(mode), juce::dontSendNotification);
                grainArpModeLabel.setText(getGrainArpModeName(mode), juce::dontSendNotification);
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

    grainArpModeLabel.setText("Octave", juce::dontSendNotification);
    grainArpModeLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    grainArpModeLabel.setJustificationType(juce::Justification::centred);
    grainArpModeLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.35f));
    addAndMakeVisible(grainArpModeLabel);
    grainArpModeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    grainArpModeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    grainArpModeSlider.setRange(0.0, 5.0, 1.0);
    grainArpModeSlider.setValue(0.0, juce::dontSendNotification);
    grainArpModeSlider.setPopupDisplayEnabled(true, false, this);
    grainArpModeSlider.textFromValueFunction = [](double v)
    {
        const int mode = juce::jlimit(0, 5, static_cast<int>(std::round(v)));
        return juce::String(mode + 1) + "/6 " + getGrainArpModeName(mode);
    };
    grainArpModeSlider.onValueChange = [this]()
    {
        const int mode = juce::jlimit(0, 5, static_cast<int>(std::round(grainArpModeSlider.getValue())));
        if (std::abs(grainArpModeSlider.getValue() - static_cast<double>(mode)) > 1.0e-6)
            grainArpModeSlider.setValue(static_cast<double>(mode), juce::dontSendNotification);
        grainArpModeLabel.setText(getGrainArpModeName(mode), juce::dontSendNotification);
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGrainArpMode(mode);
    };
    addAndMakeVisible(grainArpModeSlider);

    grainSizeSyncToggle.setButtonText("");
    grainSizeSyncToggle.setClickingTogglesState(true);
    grainSizeSyncToggle.setToggleState(false, juce::dontSendNotification);
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

    grainSizeDivLabel.setText("FREE", juce::dontSendNotification);
    grainSizeDivLabel.setJustificationType(juce::Justification::centredRight);
    grainSizeDivLabel.setColour(juce::Label::textColourId, stripColor.withAlpha(0.78f));
    grainSizeDivLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    addAndMakeVisible(grainSizeDivLabel);
    grainSizeLabel.setJustificationType(juce::Justification::centredLeft);

    patternLengthBox.addItem("16", 1);
    patternLengthBox.addItem("32", 2);
    patternLengthBox.addItem("48", 3);
    patternLengthBox.addItem("64", 4);
    patternLengthBox.setJustificationType(juce::Justification::centredLeft);
    patternLengthBox.setSelectedId(1, juce::dontSendNotification);
    patternLengthBox.setTooltip("Step pattern length");
    patternLengthBox.onChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepPatternBars(juce::jmax(1, patternLengthBox.getSelectedId()));
    };
    addAndMakeVisible(patternLengthBox);
    
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

    patternLengthLabel.setText("LEN", juce::dontSendNotification);
    patternLengthLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    patternLengthLabel.setJustificationType(juce::Justification::centred);
    patternLengthLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(patternLengthLabel);

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
    recordButton.setTooltip("Capture recent input audio into this strip (same action as monome record button).");
    recordButton.onClick = [this]()
    {
        processor.captureRecentAudioToStrip(stripIndex);
    };
    addAndMakeVisible(recordButton);

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
    modTargetBox.setSelectedId(1, juce::dontSendNotification);
    modTargetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModTarget(stripIndex, comboIdToModTarget(modTargetBox.getSelectedId()));
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

    modCurveTypeBox.addItem("Pow", 1);
    modCurveTypeBox.addItem("S", 2);
    modCurveTypeBox.addItem("Snap", 3);
    modCurveTypeBox.addItem("Stair", 4);
    modCurveTypeBox.setSelectedId(1, juce::dontSendNotification);
    modCurveTypeBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveShape(stripIndex, comboIdToCurveShape(modCurveTypeBox.getSelectedId()));
    };
    addAndMakeVisible(modCurveTypeBox);
    
    // Legacy readout removed from strip UI (kept as hidden component for compatibility).
    recordLengthLabel.setVisible(false);

    patternLengthBox.setVisible(false);
    patternLengthLabel.setVisible(false);
    updateGrainOverlayVisibility();
}

void StripControl::updateGrainOverlayVisibility()
{
    const bool isGrainMode = !showingStepDisplay
                          && processor.getAudioEngine()
                          && processor.getAudioEngine()->getStrip(stripIndex)
                          && processor.getAudioEngine()->getStrip(stripIndex)->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain;
    grainOverlayVisible = isGrainMode;

    volumeSlider.setVisible(!isGrainMode);
    panSlider.setVisible(!isGrainMode);
    volumeLabel.setVisible(!isGrainMode);
    panLabel.setVisible(!isGrainMode);

    speedSlider.setVisible(!showingStepDisplay);
    scratchSlider.setVisible(!showingStepDisplay);
    speedLabel.setVisible(!showingStepDisplay);
    scratchLabel.setVisible(!showingStepDisplay);
    recordLengthLabel.setVisible(false);

    grainSizeSlider.setVisible(isGrainMode);
    grainDensitySlider.setVisible(isGrainMode);
    grainPitchSlider.setVisible(isGrainMode);
    grainPitchJitterSlider.setVisible(isGrainMode);
    grainSpreadSlider.setVisible(isGrainMode);
    grainJitterSlider.setVisible(isGrainMode);
    grainRandomSlider.setVisible(isGrainMode);
    grainArpSlider.setVisible(isGrainMode);
    grainCloudSlider.setVisible(isGrainMode);
    grainEmitterSlider.setVisible(isGrainMode);
    grainEnvelopeSlider.setVisible(isGrainMode);
    grainArpModeSlider.setVisible(false);
    grainSizeSyncToggle.setVisible(isGrainMode);
    grainSizeDivLabel.setVisible(isGrainMode);
    grainSizeLabel.setVisible(isGrainMode);
    grainDensityLabel.setVisible(isGrainMode);
    grainPitchLabel.setVisible(isGrainMode);
    grainPitchJitterLabel.setVisible(isGrainMode);
    grainSpreadLabel.setVisible(isGrainMode);
    grainJitterLabel.setVisible(isGrainMode);
    grainRandomLabel.setVisible(isGrainMode);
    grainArpLabel.setVisible(isGrainMode);
    grainCloudLabel.setVisible(isGrainMode);
    grainEmitterLabel.setVisible(isGrainMode);
    grainEnvelopeLabel.setVisible(isGrainMode);
    grainArpModeLabel.setVisible(false);
    grainArpModeBox.setVisible(false);
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

    g.setColour(juce::Colour(0xff1f1f1f));
    g.fillRoundedRectangle(lane.toFloat(), 6.0f);
    g.setColour(stripColor.withAlpha(0.35f));
    g.drawRoundedRectangle(lane.toFloat().reduced(0.5f), 6.0f, 1.0f);

    const auto drawLane = lane.reduced(12, 2);
    const float dotSize = (totalSteps > 32) ? 4.0f : 6.0f;
    const float dotPad = dotSize * 0.6f;
    const float left = static_cast<float>(drawLane.getX()) + dotPad;
    const float right = juce::jmax(left, static_cast<float>(drawLane.getRight() - 1) - dotPad);
    const float top = static_cast<float>(drawLane.getY()) + 2.0f;
    const float bottom = static_cast<float>(drawLane.getBottom()) - 2.0f;
    const float width = right - left;
    const float height = bottom - top;
    const float xStep = juce::jmax(0.25f, width / static_cast<float>(juce::jmax(1, totalSteps - 1)));
    const float centerY = top + (height * 0.5f);

    if (seq.bipolar)
    {
        g.setColour(juce::Colour(0xff454545));
        g.drawLine(left, centerY, right, centerY, 1.0f);
    }

    std::vector<float> displayVals(static_cast<size_t>(totalSteps));
    for (int i = 0; i < totalSteps; ++i)
        displayVals[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, i));

    std::vector<juce::Point<float>> points(static_cast<size_t>(totalSteps));
    for (int i = 0; i < totalSteps; ++i)
    {
        const float v = displayVals[static_cast<size_t>(i)];
        const float n = seq.bipolar ? ((v * 2.0f) - 1.0f) : v;
        const float y = seq.bipolar
            ? (centerY - (n * (height * 0.48f)))
            : (bottom - (n * height));
        points[static_cast<size_t>(i)] = {left + (xStep * static_cast<float>(i)), y};
    }

    if (seq.curveMode)
    {
        juce::Path p;
        p.startNewSubPath(points[0]);
        const float bend = juce::jlimit(-1.0f, 1.0f, seq.curveBend);
        const auto curveShape = static_cast<ModernAudioEngine::ModCurveShape>(
            juce::jlimit(0, static_cast<int>(ModernAudioEngine::ModCurveShape::Stair), seq.curveShape));
        constexpr int segmentsPerStep = 8;
        for (int i = 0; i < (totalSteps - 1); ++i)
        {
            const float a = displayVals[static_cast<size_t>(i)];
            const float b = displayVals[static_cast<size_t>(i + 1)];
            const float x0 = points[static_cast<size_t>(i)].x;
            for (int s = 1; s <= segmentsPerStep; ++s)
            {
                const float t = static_cast<float>(s) / static_cast<float>(segmentsPerStep);
                const float shapedT = shapeCurvePhaseUi(t, bend, curveShape);
                const float v = juce::jlimit(0.0f, 1.0f, a + ((b - a) * shapedT));
                const float n = seq.bipolar ? ((v * 2.0f) - 1.0f) : v;
                const float y = seq.bipolar
                    ? (centerY - (n * (height * 0.48f)))
                    : (bottom - (n * height));
                const float x = x0 + (xStep * t);
                p.lineTo(x, y);
            }
        }

        g.setColour(stripColor.withAlpha(0.9f));
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
    else
    {
        const float barWidth = juce::jmax(2.0f, xStep * 0.68f);
        for (int i = 0; i < totalSteps; ++i)
        {
            const auto point = points[static_cast<size_t>(i)];
            const float x = point.x - (barWidth * 0.5f);
            float y0 = bottom;
            float y1 = point.y;
            if (seq.bipolar)
            {
                y0 = centerY;
                y1 = point.y;
            }
            const float yTop = juce::jmin(y0, y1);
            const float h = juce::jmax(1.0f, std::abs(y1 - y0));
            g.setColour(stripColor.withAlpha(0.55f));
            g.fillRoundedRectangle(x, yTop, barWidth, h, 1.5f);
        }
    }

    for (int i = 0; i < totalSteps; ++i)
    {
        const auto point = points[static_cast<size_t>(i)];
        const bool isActive = (i == activeStep);
        g.setColour(isActive ? kAccent : stripColor.withMultipliedBrightness(0.8f));
        g.fillEllipse(point.x - (dotSize * 0.5f), point.y - (dotSize * 0.5f), dotSize, dotSize);
    }
}

void StripControl::applyModulationPoint(juce::Point<int> p)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= 6)
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
    if (!engine || stripIndex >= 6 || modTransformStep < 0 || modTransformStep >= modTransformStepCount)
        return;

    // Cmd/Ctrl drag edits local virtual density while keeping the cycle duration fixed.
    // Drag up: more virtual steps around the selected cell.
    // Drag down: fewer virtual steps around the selected cell.
    const int sourceCount = juce::jmax(2, modTransformStepCount);
    const int stepDelta = juce::jlimit(-(sourceCount - 2), 32, (-deltaY) / 14);
    const int targetCount = juce::jlimit(2, sourceCount + 32, sourceCount + stepDelta);
    if (targetCount == sourceCount)
    {
        for (int i = 0; i < sourceCount; ++i)
            engine->setModStepValueAbsolute(stripIndex, i, modTransformSourceSteps[static_cast<size_t>(i)]);
        return;
    }

    std::vector<float> expanded;
    expanded.reserve(static_cast<size_t>(juce::jmax(sourceCount, targetCount)));
    for (int i = 0; i < sourceCount; ++i)
        expanded.push_back(modTransformSourceSteps[static_cast<size_t>(i)]);

    int pivot = juce::jlimit(0, static_cast<int>(expanded.size()) - 1, modTransformStep);
    if (targetCount > sourceCount)
    {
        const int extraNodes = targetCount - sourceCount;
        for (int n = 0; n < extraNodes; ++n)
        {
            const float v = expanded[static_cast<size_t>(pivot)];
            expanded.insert(expanded.begin() + (pivot + 1), v);
            ++pivot;
        }
    }
    else
    {
        const int removeNodes = sourceCount - targetCount;
        for (int n = 0; n < removeNodes && expanded.size() > 2; ++n)
        {
            const int left = pivot - 1;
            const int right = pivot + 1;
            int removeIdx = -1;
            if (right < static_cast<int>(expanded.size()) && left >= 0)
                removeIdx = (n % 2 == 0) ? right : left;
            else if (right < static_cast<int>(expanded.size()))
                removeIdx = right;
            else if (left >= 0)
                removeIdx = left;
            if (removeIdx < 0)
                break;
            expanded.erase(expanded.begin() + removeIdx);
            if (removeIdx < pivot)
                --pivot;
        }
    }

    const int expandedCount = static_cast<int>(expanded.size());
    if (expandedCount <= 0)
        return;

    for (int i = 0; i < sourceCount; ++i)
    {
        const double phase = (static_cast<double>(i) * static_cast<double>(expandedCount))
                           / static_cast<double>(sourceCount);
        const int idxA = juce::jlimit(0, expandedCount - 1, static_cast<int>(std::floor(phase)));
        const int idxB = (idxA + 1) % expandedCount;
        const float frac = static_cast<float>(phase - static_cast<double>(idxA));
        const float v = expanded[static_cast<size_t>(idxA)]
                      + ((expanded[static_cast<size_t>(idxB)] - expanded[static_cast<size_t>(idxA)]) * frac);
        engine->setModStepValueAbsolute(stripIndex, i, juce::jlimit(0.0f, 1.0f, v));
    }
}

void StripControl::applyModulationCellCurveFromDrag(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= 6 || modTransformStep < 0 || modTransformStep >= modTransformStepCount)
        return;

    const float srcV = modTransformSourceSteps[static_cast<size_t>(modTransformStep)];
    const float dragNorm = juce::jlimit(-1.0f, 1.0f, static_cast<float>(-deltaY) / 120.0f);
    float exponent = 1.0f; // Middle = linear
    if (dragNorm >= 0.0f)
    {
        // Drag up: progressively more exponential.
        exponent = 1.0f + (dragNorm * 5.0f); // 1 .. 6
    }
    else
    {
        // Drag down: progressively less exponential.
        exponent = 1.0f / (1.0f + ((-dragNorm) * 0.75f)); // 1 .. ~0.57
    }

    const float shaped = juce::jlimit(0.0f, 1.0f,
                                      std::pow(juce::jlimit(0.0f, 1.0f, srcV), exponent));
    engine->setModStepValueAbsolute(stripIndex, modTransformStep, shaped);
}

void StripControl::mouseDown(const juce::MouseEvent& e)
{
    if (modulationLaneView)
    {
        auto* engine = processor.getAudioEngine();
        if (!engine || stripIndex >= 6)
            return;

        const auto state = engine->getModSequencerState(stripIndex);
        const bool neutralBipolar = modTargetAllowsBipolar(state.target) && state.bipolar;
        const float neutralValue = neutralBipolar ? 0.5f : 0.0f;
        const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
        const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);

        if (e.mods.isRightButtonDown())
        {
            for (int i = 0; i < totalSteps; ++i)
                engine->setModStepValueAbsolute(stripIndex, i, neutralValue);
            modulationLastDrawStep = -1;
            return;
        }

        const auto mods = e.mods;
        const int clickedStep = getModulationStepFromPoint(e.getPosition());
        const bool duplicateGesture = mods.isCommandDown() || mods.isCtrlDown();
        const bool shapeGesture = mods.isAltDown();
        if ((duplicateGesture || shapeGesture) && clickedStep >= 0)
        {
            modTransformStepCount = totalSteps;
            for (int i = 0; i < modTransformStepCount; ++i)
                modTransformSourceSteps[static_cast<size_t>(i)] = engine->getModStepValueAbsolute(stripIndex, i);
            modTransformStartY = e.y;
            modTransformStep = clickedStep;
            modTransformMode = duplicateGesture
                ? ModTransformMode::DuplicateCell
                : ModTransformMode::ShapeCell;
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
    if (!engine || stripIndex >= 6)
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
            else if (modTransformMode == ModTransformMode::ShapeCell)
                applyModulationCellCurveFromDrag(deltaY);
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
    hide(volumeSlider); hide(panSlider); hide(speedSlider); hide(scratchSlider); hide(patternLengthBox);
    hide(tempoLabel); hide(recordBarsBox); hide(recordButton); hide(recordBarsLabel);
    hide(volumeLabel); hide(panLabel); hide(speedLabel); hide(scratchLabel); hide(patternLengthLabel);
    hide(recordLengthLabel);
}

void StripControl::hideAllGrainControls()
{
    auto hide = [](juce::Component& c){ c.setVisible(false); };
    hide(grainSizeSlider); hide(grainDensitySlider); hide(grainPitchSlider); hide(grainPitchJitterSlider);
    hide(grainSpreadSlider); hide(grainJitterSlider); hide(grainRandomSlider); hide(grainArpSlider);
    hide(grainCloudSlider); hide(grainEmitterSlider); hide(grainEnvelopeSlider); hide(grainArpModeSlider);
    hide(grainArpModeBox); hide(grainSizeSyncToggle); hide(grainSizeDivLabel); hide(grainSizeLabel);
    hide(grainDensityLabel); hide(grainPitchLabel); hide(grainPitchJitterLabel); hide(grainSpreadLabel);
    hide(grainJitterLabel); hide(grainRandomLabel); hide(grainArpLabel); hide(grainCloudLabel);
    hide(grainEmitterLabel); hide(grainEnvelopeLabel); hide(grainArpModeLabel);
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
    modulationLaneBounds = bounds.reduced(8, 0);
    
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
        modPitchQuantToggle.setVisible(true);
        modPitchScaleBox.setVisible(true);
        modShapeLabel.setVisible(true);
        modShapeBox.setVisible(true);
        modCurveTypeLabel.setVisible(true);
        modCurveTypeBox.setVisible(true);

        controlsArea.reduce(4, 0);
        const int gap = 4;
        const int columnWidth = juce::jmax(88, (controlsArea.getWidth() - gap) / 2);
        auto splitRow = [columnWidth](juce::Rectangle<int> row)
        {
            auto left = row.removeFromLeft(columnWidth);
            row.removeFromLeft(gap);
            return std::pair<juce::Rectangle<int>, juce::Rectangle<int>>(left, row);
        };

        auto row0 = controlsArea.removeFromTop(18);
        auto cols0 = splitRow(row0);
        auto row0Left = cols0.first;
        modTargetLabel.setBounds(row0Left.removeFromLeft(42));
        modTargetBox.setBounds(row0Left);
        modLengthLabel.setBounds(cols0.second.removeFromLeft(24));
        modLengthBox.setBounds(cols0.second.removeFromLeft(60));

        controlsArea.removeFromTop(2);
        auto row1 = controlsArea.removeFromTop(18);
        auto cols1 = splitRow(row1);
        auto row1Left = cols1.first;
        modDepthLabel.setBounds(row1Left.removeFromLeft(42));
        modDepthSlider.setBounds(row1Left);
        modBipolarToggle.setBounds(cols1.second);

        controlsArea.removeFromTop(2);
        auto row2 = controlsArea.removeFromTop(18);
        auto cols2 = splitRow(row2);
        auto row2Left = cols2.first;
        modOffsetLabel.setBounds(row2Left.removeFromLeft(42));
        modOffsetSlider.setBounds(row2Left);
        modCurveBendLabel.setBounds(cols2.second.removeFromLeft(34));
        modCurveBendSlider.setBounds(cols2.second);

        controlsArea.removeFromTop(2);
        auto row3 = controlsArea.removeFromTop(18);
        auto cols3 = splitRow(row3);
        modPitchQuantToggle.setBounds(cols3.first);
        modPitchScaleBox.setBounds(cols3.second);

        controlsArea.removeFromTop(2);
        auto row4 = controlsArea.removeFromTop(18);
        auto cols4 = splitRow(row4);
        modCurveTypeLabel.setBounds(cols4.first.removeFromLeft(34));
        modCurveTypeBox.setBounds(cols4.first);
        modShapeLabel.setBounds(cols4.second.removeFromLeft(34));
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
    modPitchQuantToggle.setVisible(false);
    modPitchScaleBox.setVisible(false);
    modShapeLabel.setVisible(false);
    modShapeBox.setVisible(false);
    modCurveTypeLabel.setVisible(false);
    modCurveTypeBox.setVisible(false);
    
    // Controls column on right
    controlsArea.reduce(4, 0);
    
    const bool isGrainMode = grainOverlayVisible;

    const int rowGap = isGrainMode ? 0 : 1;

    // Top row: Load + slice mode
    auto topRow = controlsArea.removeFromTop(isGrainMode ? 14 : 18);
    const int half = topRow.getWidth() / 2;
    auto loadArea = topRow.removeFromLeft(half);
    loadButton.setBounds(loadArea.reduced(0, 0));
    topRow.removeFromLeft(2);
    transientSliceButton.setBounds(topRow);
    controlsArea.removeFromTop(rowGap);
    
    // Second row: Play mode and Direction mode (2/3 width each), Group (1/3 width)
    auto modesRow = controlsArea.removeFromTop(isGrainMode ? 14 : 18);
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
        controlsArea.removeFromTop(2);
    }
    else if (showRecordBars)
    {
        auto recBarsRow = controlsArea.removeFromTop(16);
        recordBarsBox.setBounds(recBarsRow.removeFromLeft(66));
        recBarsRow.removeFromLeft(8);
        recordButton.setBounds(recBarsRow.removeFromLeft(42));
        controlsArea.removeFromTop(2);
    }
    
    // Rotary knobs row.
    auto knobsRow = controlsArea.removeFromTop(isGrainMode ? 22 : 30);
    int totalWidth = knobsRow.getWidth();
    int mainKnobsWidth = (totalWidth * 7) / 10;
    int mainKnobWidth = mainKnobsWidth / 3;

    if (isGrainMode)
    {
        grainSizeSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        grainDensitySlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        speedSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
    }
    else
    {
        volumeSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        panSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
        speedSlider.setBounds(knobsRow.removeFromLeft(mainKnobWidth).reduced(1));
    }
    knobsRow.removeFromLeft(4);
    const bool isStepMode = showingStepDisplay;
    if (isStepMode)
        patternLengthBox.setBounds(knobsRow.reduced(2));
    else
        scratchSlider.setBounds(knobsRow.reduced(2));

    auto labelsRow = controlsArea.removeFromTop(isGrainMode ? 10 : 9);
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
        speedLabel.setBounds(labelsRow.removeFromLeft(mainKnobWidth));
    }
    else
    {
        volumeLabel.setBounds(labelsRow.removeFromLeft(mainKnobWidth));
        panLabel.setBounds(labelsRow.removeFromLeft(mainKnobWidth));
        speedLabel.setBounds(labelsRow.removeFromLeft(mainKnobWidth));
    }
    labelsRow.removeFromLeft(4);
    if (isStepMode)
        patternLengthLabel.setBounds(labelsRow);
    else
        scratchLabel.setBounds(labelsRow);
    if (!isGrainMode)
    {
        // Recording loop length label (small, at bottom)
        if (controlsArea.getHeight() >= 10)
            recordLengthLabel.setBounds(controlsArea.removeFromTop(10));
        return;
    }

    // Dynamic compact grain layout to keep all controls visible.
    const int remainingH = juce::jmax(0, controlsArea.getHeight());
    const int syncRowH = juce::jlimit(6, 9, remainingH / 5);
    const int miniRowsTotal = juce::jmax(0, remainingH - syncRowH);
    const int rowH = juce::jlimit(6, 10, miniRowsTotal / 4);

    auto syncRow = controlsArea.removeFromTop(syncRowH);
    auto envArea = syncRow.removeFromRight(128);
    grainEnvelopeLabel.setBounds(envArea.removeFromLeft(30));
    grainEnvelopeSlider.setBounds(envArea);

    auto layoutGrainMiniRow = [&](int height, juce::Label& labelA, juce::Slider& sliderA,
                                  juce::Label* labelB, juce::Slider* sliderB)
    {
        if (controlsArea.getHeight() < height)
            return;
        auto row = controlsArea.removeFromTop(height);
        auto left = row.removeFromLeft(row.getWidth() / 2);
        labelA.setBounds(left.removeFromLeft(30));
        sliderA.setBounds(left);

        if (labelB != nullptr && sliderB != nullptr)
        {
            row.removeFromLeft(2);
            labelB->setBounds(row.removeFromLeft(30));
            sliderB->setBounds(row);
        }
    };

    layoutGrainMiniRow(rowH, grainPitchLabel, grainPitchSlider, &grainPitchJitterLabel, &grainPitchJitterSlider);
    layoutGrainMiniRow(rowH, grainSpreadLabel, grainSpreadSlider, &grainJitterLabel, &grainJitterSlider);
    layoutGrainMiniRow(rowH, grainRandomLabel, grainRandomSlider, &grainArpLabel, &grainArpSlider);
    layoutGrainMiniRow(rowH, grainCloudLabel, grainCloudSlider, &grainEmitterLabel, &grainEmitterSlider);
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
        patternLengthLabel.setVisible(isStepMode);
        updateGrainOverlayVisibility();
        resized();
    }
    
    // Update step display if in step mode
    if (showingStepDisplay)
    {
        stepDisplay.setStepPattern(strip->stepPattern, strip->getStepTotalSteps());
        stepDisplay.setCurrentStep(strip->currentStep);
        stepDisplay.setPlaying(strip->isPlaying());

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
        waveform.setSliceMarkers({}, {}, 0, false);
        waveform.setGrainWindowOverlay(false, 0.0);
        std::array<float, 8> emptyMarkers{};
        emptyMarkers.fill(-1.0f);
        std::array<float, 8> emptyPitch{};
        emptyPitch.fill(0.0f);
        waveform.setGrainMarkerPositions(emptyMarkers, emptyPitch);
        waveform.setGrainHudOverlay(false, {}, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
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
    
    // Sync scratch slider from engine
    scratchSlider.setValue(strip->getScratchAmount(), juce::dontSendNotification);
    patternLengthBox.setSelectedId(strip->getStepPatternBars(), juce::dontSendNotification);
    {
        float beats = strip->getBeatsPerLoop();
        if (!(beats > 0.0f && std::isfinite(beats)))
            beats = static_cast<float>(juce::jlimit(1, 8, strip->getRecordingBars()) * 4);

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
    volumeSlider.setValue(strip->getVolume(), juce::dontSendNotification);
    panSlider.setValue(strip->getPan(), juce::dontSendNotification);
    const bool showDisplaySpeed = strip->isScratchActive()
        || (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain && strip->getGrainHeldCount() > 0);
    speedSlider.setValue(showDisplaySpeed ? strip->getDisplaySpeed() : strip->getPlaybackSpeed(), juce::dontSendNotification);
    
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
    grainSizeSlider.setValue(strip->getGrainSizeMs(), juce::dontSendNotification);
    grainDensitySlider.setValue(strip->getGrainDensity(), juce::dontSendNotification);
    grainPitchSlider.setValue(strip->getGrainPitch(), juce::dontSendNotification);
    grainPitchJitterSlider.setValue(strip->getGrainPitchJitter(), juce::dontSendNotification);
    grainSpreadSlider.setValue(strip->getGrainSpread(), juce::dontSendNotification);
    grainJitterSlider.setValue(strip->getGrainJitter(), juce::dontSendNotification);
    grainRandomSlider.setValue(strip->getGrainRandomDepth(), juce::dontSendNotification);
    grainArpSlider.setValue(strip->getGrainArpDepth(), juce::dontSendNotification);
    grainCloudSlider.setValue(strip->getGrainCloudDepth(), juce::dontSendNotification);
    grainEmitterSlider.setValue(strip->getGrainEmitterDepth(), juce::dontSendNotification);
    grainEnvelopeSlider.setValue(strip->getGrainEnvelope(), juce::dontSendNotification);
    if (!grainArpModeSlider.isMouseButtonDown())
        grainArpModeSlider.setValue(strip->getGrainArpMode(), juce::dontSendNotification);
    {
        if (grainArpModeSlider.isMouseButtonDown())
            strip->setGrainArpMode(juce::jlimit(0, 5, static_cast<int>(std::round(grainArpModeSlider.getValue()))));
        const int arpMode = juce::jlimit(0, 5, static_cast<int>(std::round(grainArpModeSlider.getValue())));
        grainArpModeLabel.setText(getGrainArpModeName(arpMode), juce::dontSendNotification);
    }
    const bool grainSyncEnabled = strip->isGrainTempoSyncEnabled();
    grainSizeSyncToggle.setToggleState(grainSyncEnabled, juce::dontSendNotification);
    grainSizeDivLabel.setText(grainSyncEnabled ? "SYNC" : "FREE", juce::dontSendNotification);
    grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    {
        const bool arpActive = strip->getGrainArpDepth() > 0.001f;
        grainPitchLabel.setText(arpActive ? "RANGE" : "PITCH", juce::dontSendNotification);
        if (arpActive)
        {
            grainPitchSlider.setRange(0.0, 48.0, 0.1);
            grainPitchSlider.setValue(std::abs(strip->getGrainPitch()), juce::dontSendNotification);
        }
        else
        {
            grainPitchSlider.setRange(-48.0, 48.0, 0.1);
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
    tintSlider(speedSlider, baseControl, 0.0f);
    tintSlider(scratchSlider, baseControl, 0.0f);
    tintSlider(grainSizeSlider, baseControl, 0.0f);
    tintSlider(grainDensitySlider, baseControl, 0.0f);
    tintSlider(grainPitchSlider, baseControl, 0.0f);
    tintSlider(grainPitchJitterSlider, baseControl, 0.0f);
    tintSlider(grainSpreadSlider, baseControl, 0.0f);
    tintSlider(grainJitterSlider, baseControl, 0.0f);
    tintSlider(grainRandomSlider, baseControl, 0.0f);
    tintSlider(grainArpSlider, baseControl, 0.0f);
    tintSlider(grainCloudSlider, baseControl, 0.0f);
    tintSlider(grainEmitterSlider, baseControl, 0.0f);
    tintSlider(grainEnvelopeSlider, baseControl, 0.0f);
    setModIndicator(volumeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(panSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(speedSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(scratchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSizeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainDensitySlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSpreadSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainRandomSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainArpSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainCloudSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEmitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEnvelopeSlider, false, 0.0f, 0.0f, kAccent);

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
                    case ModernAudioEngine::ModTarget::Pitch: return nullptr;
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
            strip->setFilterEnabled(filterEnableButton.getToggleState());
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
            strip->setFilterFrequency(static_cast<float>(filterFreqSlider.getValue()));
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
            strip->setFilterResonance(static_cast<float>(filterResSlider.getValue()));
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
            strip->setFilterMorph(static_cast<float>(filterMorphSlider.getValue()));
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
    filterAlgoBox.addItem("MOOG H", 6);
    filterAlgoBox.setSelectedId(1);
    styleUiCombo(filterAlgoBox);
    filterAlgoBox.setJustificationType(juce::Justification::centred);
    filterAlgoBox.setTooltip("Filter algorithm: SVF12, SVF24, Ladder12, Ladder24, Moog Stilson LP, Moog Huovilainen LP");
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

    gateSpeedLabel.setText("Rate", juce::dontSendNotification);
    gateSpeedLabel.setJustificationType(juce::Justification::centredLeft);
    gateSpeedLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    gateSpeedLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(gateSpeedLabel);

    for (int i = 0; i < static_cast<int>(kGateRates.size()); ++i)
        gateSpeedBox.addItem(kGateRates[static_cast<size_t>(i)].label, i + 1);
    gateSpeedBox.setSelectedId(gateRateIdFromCycles(4.0f), juce::dontSendNotification);
    gateSpeedBox.onChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGateSpeed(gateRateCyclesFromId(gateSpeedBox.getSelectedId()));
    };
    addAndMakeVisible(gateSpeedBox);

    gateEnvLabel.setText("Env", juce::dontSendNotification);
    gateEnvLabel.setJustificationType(juce::Justification::centredLeft);
    gateEnvLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    gateEnvLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(gateEnvLabel);

    gateEnvSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gateEnvSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 14);
    gateEnvSlider.setRange(0.0, 1.0, 0.01);
    gateEnvSlider.setValue(0.5);
    gateEnvSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setGateEnvelope(static_cast<float>(gateEnvSlider.getValue()));
    };
    addAndMakeVisible(gateEnvSlider);

    gateShapeLabel.setText("Shape", juce::dontSendNotification);
    gateShapeLabel.setJustificationType(juce::Justification::centredLeft);
    gateShapeLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    gateShapeLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(gateShapeLabel);

    gateShapeBox.addItem("Sine", 1);
    gateShapeBox.addItem("Triangle", 2);
    gateShapeBox.addItem("Square", 3);
    gateShapeBox.setSelectedId(1);
    gateShapeBox.onChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            const int id = gateShapeBox.getSelectedId();
            EnhancedAudioStrip::GateShape shape = EnhancedAudioStrip::GateShape::Sine;
            if (id == 2) shape = EnhancedAudioStrip::GateShape::Triangle;
            else if (id == 3) shape = EnhancedAudioStrip::GateShape::Square;
            strip->setGateShape(shape);
        }
    };
    addAndMakeVisible(gateShapeBox);
    
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
    
    // No strip label - just controls
    
    // SPLIT INTO THREE FIELDS
    int fieldWidth = bounds.getWidth() / 3;
    
    // Field 1: Filter controls (left third)
    auto field1 = bounds.removeFromLeft(fieldWidth).reduced(6, 0);
    
    // Field 2: gate controls, field 3 reserved for future effect
    auto field2 = bounds.removeFromLeft(fieldWidth).reduced(6, 0);
    auto field3 = bounds.reduced(6, 0);
    
    // === FIELD 1: FILTER CONTROLS (ALL IN ONE ROW) ===
    
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

    // === FIELD 2: GATE CONTROLS ===
    auto rateRow = field2.removeFromTop(20);
    gateSpeedLabel.setBounds(rateRow.removeFromLeft(38));
    gateSpeedBox.setBounds(rateRow);
    field2.removeFromTop(4);

    auto envRow = field2.removeFromTop(20);
    gateEnvLabel.setBounds(envRow.removeFromLeft(38));
    gateEnvSlider.setBounds(envRow);
    field2.removeFromTop(4);

    auto shapeRow = field2.removeFromTop(20);
    gateShapeLabel.setBounds(shapeRow.removeFromLeft(38));
    gateShapeBox.setBounds(shapeRow);

    // === FIELD 3: RESERVED ===
    (void) field3;
}

void FXStripControl::updateFromEngine()
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip) return;
    
    // Update from engine state
    filterEnableButton.setToggleState(strip->isFilterEnabled(), juce::dontSendNotification);
    filterFreqSlider.setValue(strip->getFilterFrequency(), juce::dontSendNotification);
    filterResSlider.setValue(strip->getFilterResonance(), juce::dontSendNotification);
    filterMorphSlider.setValue(strip->getFilterMorph(), juce::dontSendNotification);
    gateSpeedBox.setSelectedId(gateRateIdFromCycles(strip->getGateSpeed()), juce::dontSendNotification);
    gateEnvSlider.setValue(strip->getGateEnvelope(), juce::dontSendNotification);

    const auto algo = strip->getFilterAlgorithm();
    int algoId = 1;
    if (algo == EnhancedAudioStrip::FilterAlgorithm::Tpt24) algoId = 2;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder12) algoId = 3;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder24) algoId = 4;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogStilson) algoId = 5;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogHuov) algoId = 6;
    filterAlgoBox.setSelectedId(algoId, juce::dontSendNotification);
    int gateShapeId = 1;
    switch (strip->getGateShape())
    {
        case EnhancedAudioStrip::GateShape::Triangle: gateShapeId = 2; break;
        case EnhancedAudioStrip::GateShape::Square: gateShapeId = 3; break;
        case EnhancedAudioStrip::GateShape::Sine:
        default: gateShapeId = 1; break;
    }
    gateShapeBox.setSelectedId(gateShapeId, juce::dontSendNotification);
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

    swingDivisionLabel.setText("Swing grid", juce::dontSendNotification);
    swingDivisionLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(swingDivisionLabel);

    swingDivisionBox.addItem("1/4", 1);
    swingDivisionBox.addItem("1/8", 2);
    swingDivisionBox.addItem("1/16", 3);
    swingDivisionBox.addItem("Triplet", 4);
    swingDivisionBox.onChange = [this]()
    {
        processor.setSwingDivisionSelection(swingDivisionBox.getSelectedId() - 1);
    };
    addAndMakeVisible(swingDivisionBox);
    styleUiCombo(swingDivisionBox);

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
    };
    momentaryToggle.setTooltip("Monome page buttons are hold-to-temporary when enabled.");
    addAndMakeVisible(momentaryToggle);
    styleUiButton(momentaryToggle);
    
    crossfadeLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "crossfadeLength", crossfadeLengthSlider);
    triggerFadeInAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "triggerFadeIn", triggerFadeInSlider);

    refreshFromProcessor();
}

//==============================================================================
// PresetControlPanel Implementation
//==============================================================================

PresetControlPanel::PresetControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    instructionsLabel.setText("Click=load  Shift+Click=save  Delete removes selected slot", juce::dontSendNotification);
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
    if (!processor.presetExists(index))
        return;

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
    momentaryToggle.setBounds(titleRow.removeFromRight(92));
    titleRow.removeFromRight(6);
    titleLabel.setBounds(titleRow);
    
    bounds.removeFromTop(4);  // Less gap (was 8)
    
    auto controlsArea = bounds;
    
    // COMPACT LAYOUT: Reduce individual column widths
    // Calculate based on actual needs rather than equal division
    const int sliderWidth = 50;      // Vertical sliders
    const int meterWidth = 30;       // L/R meters
    const int knobWidth = 112;       // Larger rotary fade controls
    const int dropdownWidth = 92;    // Dropdowns
    const int spacing = 8;           // Between controls
    
    // Master volume
    auto masterArea = controlsArea.removeFromLeft(sliderWidth);
    masterVolumeLabel.setBounds(masterArea.removeFromTop(16));  // Smaller label (was 20)
    masterArea.removeFromTop(2);  // Less gap (was 4)
    masterVolumeSlider.setBounds(masterArea);
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

    auto outputRoutingArea = controlsArea.removeFromLeft(132);
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
    targetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModTarget(selectedStrip, comboIdToModTarget(targetBox.getSelectedId()));
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
            engine->setModLengthBars(selectedStrip, bars);
            const int currentPage = juce::jlimit(0, bars - 1, engine->getModCurrentPage(selectedStrip));
            engine->setModEditPage(selectedStrip, currentPage);
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

    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        b.setButtonText(juce::String(i + 1));
        b.onClick = [this, i]()
        {
            if (suppressNextStepClick)
            {
                suppressNextStepClick = false;
                return;
            }
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
    if (!processor.getAudioEngine())
        return;

    const int step = stepIndexForComponent(e.eventComponent);
    if (step < 0)
        return;

    if (e.mods.isCommandDown() || e.mods.isAltDown())
    {
        const auto state = processor.getAudioEngine()->getModSequencerState(selectedStrip);
        gestureSourceSteps = state.steps;
        gestureMode = e.mods.isCommandDown() ? EditGestureMode::DuplicateCell : EditGestureMode::ShapeCell;
        gestureActive = true;
        gestureStartY = e.getScreenPosition().y;
        gestureStep = step;
        suppressNextStepClick = true;
    }
}

void ModulationControlPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (!gestureActive || !processor.getAudioEngine())
        return;

    const int deltaY = e.getScreenPosition().y - gestureStartY;
    if (gestureMode == EditGestureMode::DuplicateCell)
        applyDuplicateGesture(deltaY);
    else if (gestureMode == EditGestureMode::ShapeCell)
        applyShapeGesture(deltaY);

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

    const int stepDelta = juce::jlimit(-(ModernAudioEngine::ModSteps - 2), 32, (-deltaY) / 14);
    const int targetCount = juce::jlimit(2, ModernAudioEngine::ModSteps + 32, ModernAudioEngine::ModSteps + stepDelta);
    if (targetCount == ModernAudioEngine::ModSteps)
    {
        for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
            engine->setModStepValue(selectedStrip, i, gestureSourceSteps[static_cast<size_t>(i)]);
        return;
    }

    std::vector<float> expanded;
    expanded.reserve(static_cast<size_t>(juce::jmax(ModernAudioEngine::ModSteps, targetCount)));
    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
        expanded.push_back(gestureSourceSteps[static_cast<size_t>(i)]);

    int pivot = juce::jlimit(0, static_cast<int>(expanded.size()) - 1, gestureStep);
    if (targetCount > ModernAudioEngine::ModSteps)
    {
        const int extraNodes = targetCount - ModernAudioEngine::ModSteps;
        for (int n = 0; n < extraNodes; ++n)
        {
            const float v = expanded[static_cast<size_t>(pivot)];
            expanded.insert(expanded.begin() + (pivot + 1), v);
            ++pivot;
        }
    }
    else
    {
        const int removeNodes = ModernAudioEngine::ModSteps - targetCount;
        for (int n = 0; n < removeNodes && expanded.size() > 2; ++n)
        {
            const int left = pivot - 1;
            const int right = pivot + 1;
            int removeIdx = -1;
            if (right < static_cast<int>(expanded.size()) && left >= 0)
                removeIdx = (n % 2 == 0) ? right : left;
            else if (right < static_cast<int>(expanded.size()))
                removeIdx = right;
            else if (left >= 0)
                removeIdx = left;
            if (removeIdx < 0)
                break;
            expanded.erase(expanded.begin() + removeIdx);
            if (removeIdx < pivot)
                --pivot;
        }
    }

    const int expandedCount = static_cast<int>(expanded.size());
    if (expandedCount <= 0)
        return;

    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        const double phase = (static_cast<double>(i) * static_cast<double>(expandedCount))
                           / static_cast<double>(ModernAudioEngine::ModSteps);
        const int idxA = juce::jlimit(0, expandedCount - 1, static_cast<int>(std::floor(phase)));
        const int idxB = (idxA + 1) % expandedCount;
        const float frac = static_cast<float>(phase - static_cast<double>(idxA));
        const float v = expanded[static_cast<size_t>(idxA)]
                      + ((expanded[static_cast<size_t>(idxB)] - expanded[static_cast<size_t>(idxA)]) * frac);
        engine->setModStepValue(selectedStrip, i, juce::jlimit(0.0f, 1.0f, v));
    }
}

void ModulationControlPanel::applyShapeGesture(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || gestureStep < 0 || gestureStep >= ModernAudioEngine::ModSteps)
        return;

    const float srcV = gestureSourceSteps[static_cast<size_t>(gestureStep)];
    const float dragNorm = juce::jlimit(-1.0f, 1.0f, static_cast<float>(-deltaY) / 120.0f);
    float exponent = 1.0f;
    if (dragNorm >= 0.0f)
        exponent = 1.0f + (dragNorm * 5.0f);
    else
        exponent = 1.0f / (1.0f + ((-dragNorm) * 0.75f));

    const float shaped = juce::jlimit(0.0f, 1.0f, std::pow(juce::jlimit(0.0f, 1.0f, srcV), exponent));
    engine->setModStepValue(selectedStrip, gestureStep, shaped);
}

void ModulationControlPanel::refreshFromEngine()
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    selectedStrip = juce::jlimit(0, 5, processor.getLastMonomePressedStripRow());
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

    const int activeStep = engine->getModCurrentStep(selectedStrip);
    for (int i = 0; i < ModernAudioEngine::ModSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        const bool on = state.steps[static_cast<size_t>(i)] >= 0.5f;
        juce::Colour c = on ? kAccent.withMultipliedBrightness(0.9f) : juce::Colour(0xff2f2f2f);
        if (i == activeStep)
            c = on ? juce::Colour(0xffffcf75) : juce::Colour(0xff5a4a2f);
        b.setColour(juce::TextButton::buttonColourId, c);
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
    
    // Enable keyboard input for spacebar transport control
    setWantsKeyboardFocus(true);
    
    // Set window size FIRST
    setSize(windowWidth, windowHeight);
    setResizable(true, true);
    setResizeLimits(1000, 900, 1920, 1400);  // Larger minimum for tempo controls
    
    // Create all UI components
    createUIComponents();
    
    // Force initial layout
    resized();
    
    // Start UI update timer
    startTimer(50);
    lastPresetRefreshToken = audioProcessor.getPresetRefreshToken();
}

void MlrVSTAudioProcessorEditor::createUIComponents()
{
    constexpr int kVisibleSampleStrips = 6;
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
        int maxStrips;
        
        PlayPanel(juce::OwnedArray<StripControl>& s, int max) : strips(s), maxStrips(max) {}
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            const int gap = 1;
            int stripHeight = (bounds.getHeight() - (gap * (strips.size() - 1))) / strips.size();
            stripHeight = juce::jmax(122, stripHeight);  // Larger controls while keeping strip 6 visible
            
            for (int i = 0; i < strips.size(); ++i)
            {
                int y = i * (stripHeight + gap);
                strips[i]->setBounds(0, y, bounds.getWidth(), stripHeight);
            }
        }
    };
    
    struct FXPanel : public juce::Component
    {
        juce::OwnedArray<FXStripControl>& strips;
        int maxStrips;
        
        FXPanel(juce::OwnedArray<FXStripControl>& s, int max) : strips(s), maxStrips(max) {}
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            const int gap = 1;
            int stripHeight = (bounds.getHeight() - (gap * (strips.size() - 1))) / strips.size();
            stripHeight = juce::jmax(122, stripHeight);  // Match strip height policy for FX tab
            
            for (int i = 0; i < strips.size(); ++i)
            {
                int y = i * (stripHeight + gap);
                strips[i]->setBounds(0, y, bounds.getWidth(), stripHeight);
            }
        }
    };
    
    // Create MAIN UNIFIED TABS: Play / FX / Patterns / Groups
    mainTabs = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    
    // PLAY TAB - regular strip controls
    auto* playPanel = new PlayPanel(stripControls, kVisibleSampleStrips);
    for (int i = 0; i < kVisibleSampleStrips; ++i)
    {
        auto* strip = new StripControl(i, audioProcessor);
        stripControls.add(strip);
        playPanel->addAndMakeVisible(strip);
    }
    
    // FX TAB - filter controls for each strip
    auto* fxPanel = new FXPanel(fxStripControls, kVisibleSampleStrips);
    for (int i = 0; i < kVisibleSampleStrips; ++i)
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
    for (int i = 0; i < stripControls.size(); ++i)
    {
        if (auto* strip = stripControls[i])
        {
            const bool showLane = modulationActive && i < 6;
            strip->setModulationLaneView(showLane);
            strip->setVisible(!modulationActive || i < 6);
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
