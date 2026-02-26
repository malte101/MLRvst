#include "PluginProcessor.h"
#include <array>
#include <cmath>

namespace
{
constexpr float kFilterMinHz = 20.0f;
constexpr float kFilterMaxHz = 20000.0f;
constexpr float kFilterNormBase = 1000.0f; // 20Hz * 1000 == 20kHz

float normalizeFilterHz(float hz)
{
    const float clamped = juce::jlimit(kFilterMinHz, kFilterMaxHz, hz);
    return juce::jlimit(0.0f, 1.0f, std::log(clamped / kFilterMinHz) / std::log(kFilterNormBase));
}

float denormalizeFilterHz(float normalized)
{
    const float n = juce::jlimit(0.0f, 1.0f, normalized);
    return juce::jlimit(kFilterMinHz, kFilterMaxHz, kFilterMinHz * std::pow(kFilterNormBase, n));
}

int stepToRingIndex(int step)
{
    return juce::jlimit(0, 63, (juce::jlimit(0, 15, step) * 4) + 1);
}

void overlayIndicator(std::array<int, 64>& ring, int ringIndex)
{
    const int idx = juce::jlimit(0, 63, ringIndex);
    const int prev = (idx + 63) % 64;
    const int next = (idx + 1) % 64;
    ring[static_cast<size_t>(idx)] = juce::jmax(ring[static_cast<size_t>(idx)], 15);
    ring[static_cast<size_t>(prev)] = juce::jmax(ring[static_cast<size_t>(prev)], 10);
    ring[static_cast<size_t>(next)] = juce::jmax(ring[static_cast<size_t>(next)], 10);
}

std::array<int, 64> makeAbsoluteRing(float normalized)
{
    std::array<int, 64> ring{};
    ring.fill(0);

    const float clamped = juce::jlimit(0.0f, 1.0f, normalized);
    const int marker = juce::jlimit(0, 63, static_cast<int>(std::round(clamped * 63.0f)));
    for (int i = 0; i <= marker; ++i)
    {
        const float t = (marker > 0) ? (static_cast<float>(i) / static_cast<float>(marker)) : 1.0f;
        const int level = juce::jlimit(2, 14, static_cast<int>(std::round(3.0f + (9.0f * t))));
        ring[static_cast<size_t>(i)] = level;
    }
    ring[static_cast<size_t>(marker)] = 15;
    return ring;
}

std::array<int, 64> makeBipolarRing(float bipolarValue)
{
    std::array<int, 64> ring{};
    ring.fill(0);

    const float clamped = juce::jlimit(-1.0f, 1.0f, bipolarValue);
    const int center = 32;
    const int marker = juce::jlimit(0, 63, static_cast<int>(std::round((clamped * 31.0f) + 32.0f)));
    const int start = juce::jmin(center, marker);
    const int end = juce::jmax(center, marker);
    for (int i = start; i <= end; ++i)
    {
        const float t = (end > start) ? (static_cast<float>(i - start) / static_cast<float>(end - start)) : 1.0f;
        const int level = juce::jlimit(2, 14, static_cast<int>(std::round(4.0f + (8.0f * t))));
        ring[static_cast<size_t>(i)] = level;
    }
    ring[static_cast<size_t>(center)] = juce::jmax(ring[static_cast<size_t>(center)], 9);
    ring[static_cast<size_t>(marker)] = 15;
    return ring;
}

std::array<int, 64> makeStepSelectRing(int selectedStep)
{
    std::array<int, 64> ring{};
    ring.fill(0);

    const int clampedStep = juce::jlimit(0, 15, selectedStep);
    for (int step = 0; step < 16; ++step)
    {
        const int start = step * 4;
        const int level = (step <= clampedStep) ? 6 : 2;
        for (int i = 0; i < 4; ++i)
            ring[static_cast<size_t>(start + i)] = level;
    }

    const int selectedStart = clampedStep * 4;
    for (int i = 0; i < 4; ++i)
        ring[static_cast<size_t>(selectedStart + i)] = 15;
    return ring;
}

std::array<int, 64> makeSubdivisionRing(int subdivisions)
{
    std::array<int, 64> ring{};
    ring.fill(0);

    const int clamped = juce::jlimit(1, ModernAudioEngine::ModMaxStepSubdivisions, subdivisions);
    for (int sub = 0; sub < ModernAudioEngine::ModMaxStepSubdivisions; ++sub)
    {
        const int segmentStart = sub * 4;
        if (sub < clamped)
        {
            const int level = juce::jlimit(3, 14, 6 + static_cast<int>(std::round((7.0f * static_cast<float>(sub + 1))
                / static_cast<float>(ModernAudioEngine::ModMaxStepSubdivisions))));
            for (int i = 0; i < 4; ++i)
                ring[static_cast<size_t>(segmentStart + i)] = level;
        }
        else
        {
            ring[static_cast<size_t>(segmentStart)] = 1;
        }
    }

    const int selectedStart = (clamped - 1) * 4;
    for (int i = 0; i < 4; ++i)
        ring[static_cast<size_t>(selectedStart + i)] = 15;
    return ring;
}
}

bool MlrVSTAudioProcessor::isArcModulationMode() const
{
    return (arcControlMode == ArcControlMode::Modulation)
        || (controlModeActive && currentControlMode == ControlMode::Modulation);
}

void MlrVSTAudioProcessor::setArcControlMode(ArcControlMode mode)
{
    if (arcControlMode == mode)
        return;

    arcControlMode = mode;
    for (auto& ring : arcRingCache)
        ring.fill(-1);

    if (audioEngine && isArcModulationMode())
    {
        const int targetStrip = juce::jlimit(0, MaxStrips - 1, getLastMonomePressedStripRow());
        arcSelectedModStep = juce::jlimit(0, ModernAudioEngine::ModSteps - 1, audioEngine->getModCurrentStep(targetStrip));
    }
}

void MlrVSTAudioProcessor::handleMonomeArcKey(int encoder, int state)
{
    const int clampedEncoder = juce::jlimit(0, 3, encoder);
    const bool isDown = (state != 0);
    const bool wasDown = (arcKeyHeld[static_cast<size_t>(clampedEncoder)] != 0);
    arcKeyHeld[static_cast<size_t>(clampedEncoder)] = isDown ? 1 : 0;

    // Encoder-key 0 toggles ARC mode between SelectedStrip and Modulation.
    if (clampedEncoder == 0 && isDown && !wasDown)
    {
        const auto next = (arcControlMode == ArcControlMode::SelectedStrip)
            ? ArcControlMode::Modulation
            : ArcControlMode::SelectedStrip;
        setArcControlMode(next);
        updateMonomeArcRings();
    }
}

void MlrVSTAudioProcessor::handleMonomeArcDelta(int encoder, int delta)
{
    if (!audioEngine || !monomeConnection.isConnected() || !monomeConnection.supportsArc() || delta == 0)
        return;

    const int clampedEncoder = juce::jlimit(0, 3, encoder);
    const bool fineAdjust = (arcKeyHeld[static_cast<size_t>(clampedEncoder)] != 0) && clampedEncoder != 0;
    const int targetStrip = juce::jlimit(0, MaxStrips - 1, getLastMonomePressedStripRow());
    auto* strip = audioEngine->getStrip(targetStrip);
    const bool modulationMode = isArcModulationMode();

    auto notifyStripParam = [this, targetStrip](const juce::String& paramId, float value)
    {
        if (auto* param = parameters.getParameter(paramId + juce::String(targetStrip)))
        {
            const float normalized = juce::jlimit(0.0f, 1.0f, param->convertTo0to1(value));
            param->setValueNotifyingHost(normalized);
        }
    };

    if (modulationMode)
    {
        const int activePage = audioEngine->getModEditPage(targetStrip);
        arcSelectedModStep = juce::jlimit(0, ModernAudioEngine::ModSteps - 1, arcSelectedModStep);
        const int absoluteStep = (activePage * ModernAudioEngine::ModSteps) + arcSelectedModStep;

        switch (clampedEncoder)
        {
            case 0:
                arcSelectedModStep = juce::jlimit(0, ModernAudioEngine::ModSteps - 1, arcSelectedModStep + delta);
                break;

            case 1:
            {
                const float currentStart = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepValueAbsolute(targetStrip, absoluteStep));
                const float currentEnd = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepEndValueAbsolute(targetStrip, absoluteStep));
                const int subdivisions = audioEngine->getModStepSubdivisionAbsolute(targetStrip, absoluteStep);
                const auto stepCurveShape = audioEngine->getModStepCurveShapeAbsolute(targetStrip, absoluteStep);
                const float stepSize = fineAdjust ? 0.0025f : 0.0100f;
                const float updatedStart = juce::jlimit(0.0f, 1.0f, currentStart + (static_cast<float>(delta) * stepSize));

                if (std::abs(updatedStart - currentStart) > 1.0e-6f)
                {
                    const float endDelta = currentEnd - currentStart;
                    audioEngine->setModStepValueAbsolute(targetStrip, absoluteStep, updatedStart);
                    audioEngine->setModStepCurveShapeAbsolute(targetStrip, absoluteStep, stepCurveShape);
                    if (subdivisions > 1)
                    {
                        const float updatedEnd = juce::jlimit(0.0f, 1.0f, updatedStart + endDelta);
                        audioEngine->setModStepShapeAbsolute(targetStrip, absoluteStep, subdivisions, updatedEnd);
                    }
                }
                break;
            }

            case 2:
            {
                int subDelta = delta;
                if (fineAdjust)
                {
                    subDelta /= 2;
                    if (subDelta == 0)
                        subDelta = (delta > 0) ? 1 : -1;
                }

                const int currentSubdivisions = audioEngine->getModStepSubdivisionAbsolute(targetStrip, absoluteStep);
                const int nextSubdivisions = juce::jlimit(
                    1, ModernAudioEngine::ModMaxStepSubdivisions, currentSubdivisions + subDelta);
                if (nextSubdivisions != currentSubdivisions)
                {
                    const float startValue = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepValueAbsolute(targetStrip, absoluteStep));
                    float endValue = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepEndValueAbsolute(targetStrip, absoluteStep));
                    if (currentSubdivisions <= 1 && nextSubdivisions > 1)
                    {
                        const int nextStep = (arcSelectedModStep + 1) % ModernAudioEngine::ModSteps;
                        endValue = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepValue(targetStrip, nextStep));
                    }
                    if (nextSubdivisions <= 1)
                        endValue = startValue;

                    audioEngine->setModStepShapeAbsolute(targetStrip, absoluteStep, nextSubdivisions, endValue);
                }
                break;
            }

            case 3:
            {
                const bool smoothingGesture = (arcKeyHeld[3] != 0);
                if (smoothingGesture)
                {
                    const float smoothMs = audioEngine->getModSmoothingMs(targetStrip);
                    const float smoothStep = fineAdjust ? 0.5f : 2.0f;
                    audioEngine->setModSmoothingMs(targetStrip,
                                                   juce::jlimit(0.0f, 250.0f, smoothMs + (static_cast<float>(delta) * smoothStep)));
                }
                else
                {
                    const float bend = audioEngine->getModCurveBend(targetStrip);
                    const float bendStep = fineAdjust ? 0.010f : 0.045f;
                    audioEngine->setModCurveBend(targetStrip,
                                                 juce::jlimit(-1.0f, 1.0f, bend + (static_cast<float>(delta) * bendStep)));
                }
                break;
            }
            default:
                break;
        }
    }
    else
    {
        if (!strip)
            return;

        const bool grainMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
        switch (clampedEncoder)
        {
            case 0:
            {
                if (grainMode)
                {
                    const float stepMs = fineAdjust ? 2.0f : 10.0f;
                    strip->setGrainSizeMs(strip->getGrainSizeMs() + (static_cast<float>(delta) * stepMs));
                }
                else
                {
                    const float speedStep = fineAdjust ? 0.01f : 0.04f;
                    const float next = juce::jlimit(0.0f, 4.0f, strip->getPlaybackSpeed() + (static_cast<float>(delta) * speedStep));
                    strip->setPlaybackSpeed(next);
                    notifyStripParam("stripSpeed", next);
                }
                break;
            }

            case 1:
            {
                if (grainMode)
                {
                    const float densityStep = fineAdjust ? 0.003f : 0.012f;
                    strip->setGrainDensity(strip->getGrainDensity() + (static_cast<float>(delta) * densityStep));
                }
                else
                {
                    const float pitchStep = fineAdjust ? 0.10f : 0.35f;
                    const float next = juce::jlimit(-24.0f, 24.0f, strip->getPitchShift() + (static_cast<float>(delta) * pitchStep));
                    strip->setPitchShift(next);
                    notifyStripParam("stripPitch", next);
                }
                break;
            }

            case 2:
            {
                if (grainMode)
                {
                    const float pitchStep = fineAdjust ? 0.10f : 0.35f;
                    strip->setGrainPitch(strip->getGrainPitch() + (static_cast<float>(delta) * pitchStep));
                }
                else
                {
                    const float currentNorm = normalizeFilterHz(strip->getFilterFrequency());
                    const float filterStep = fineAdjust ? 0.003f : 0.012f;
                    const float nextNorm = juce::jlimit(0.0f, 1.0f, currentNorm + (static_cast<float>(delta) * filterStep));
                    strip->setFilterEnabled(true);
                    strip->setFilterFrequency(denormalizeFilterHz(nextNorm));
                }
                break;
            }

            case 3:
            {
                if (grainMode)
                {
                    const float spreadStep = fineAdjust ? 0.003f : 0.012f;
                    strip->setGrainSpread(strip->getGrainSpread() + (static_cast<float>(delta) * spreadStep));
                }
                else
                {
                    const float depthStep = fineAdjust ? 0.005f : 0.020f;
                    audioEngine->setModDepth(targetStrip,
                                             juce::jlimit(0.0f, 1.0f,
                                                          audioEngine->getModDepth(targetStrip) + (static_cast<float>(delta) * depthStep)));
                }
                break;
            }
            default:
                break;
        }
    }

    updateMonomeArcRings();
    if (monomeConnection.supportsGrid())
        updateMonomeLEDs();
}

void MlrVSTAudioProcessor::updateMonomeArcRings()
{
    if (!audioEngine || !monomeConnection.isConnected() || !monomeConnection.supportsArc())
        return;

    const int ringCount = juce::jlimit(0, 4, monomeConnection.getArcEncoderCount());
    if (ringCount <= 0)
        return;

    const int targetStrip = juce::jlimit(0, MaxStrips - 1, getLastMonomePressedStripRow());
    auto* strip = audioEngine->getStrip(targetStrip);
    const bool modulationMode = isArcModulationMode();
    const bool hasStrip = (strip != nullptr);
    const bool grainMode = hasStrip && (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);

    auto sendRingIfChanged = [this](int encoder, const std::array<int, 64>& ring)
    {
        if (encoder < 0 || encoder >= static_cast<int>(arcRingCache.size()))
            return;

        auto& cache = arcRingCache[static_cast<size_t>(encoder)];
        if (cache == ring)
            return;

        monomeConnection.setArcRingMap(encoder, ring);
        cache = ring;
    };

    int overlayStep = -1;
    if (modulationMode)
    {
        const int activePage = audioEngine->getModEditPage(targetStrip);
        arcSelectedModStep = juce::jlimit(0, ModernAudioEngine::ModSteps - 1, arcSelectedModStep);
        const int absoluteStep = (activePage * ModernAudioEngine::ModSteps) + arcSelectedModStep;
        const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(targetStrip);
        const int playbackPage = juce::jlimit(
            0,
            ModernAudioEngine::MaxModBars - 1,
            activeGlobalStep / ModernAudioEngine::ModSteps);
        if (playbackPage == activePage)
            overlayStep = juce::jlimit(0, ModernAudioEngine::ModSteps - 1, activeGlobalStep % ModernAudioEngine::ModSteps);

        const auto seq = audioEngine->getModSequencerState(targetStrip);
        const float stepValue = juce::jlimit(0.0f, 1.0f, audioEngine->getModStepValueAbsolute(targetStrip, absoluteStep));
        const int subdivisions = audioEngine->getModStepSubdivisionAbsolute(targetStrip, absoluteStep);
        const bool smoothingPreview = (arcKeyHeld[3] != 0);

        if (ringCount >= 1)
            sendRingIfChanged(0, makeStepSelectRing(arcSelectedModStep));

        if (ringCount >= 2)
        {
            const auto ring = seq.bipolar ? makeBipolarRing((stepValue * 2.0f) - 1.0f)
                                          : makeAbsoluteRing(stepValue);
            sendRingIfChanged(1, ring);
        }

        if (ringCount >= 3)
            sendRingIfChanged(2, makeSubdivisionRing(subdivisions));

        if (ringCount >= 4)
        {
            const auto ring = smoothingPreview
                ? makeAbsoluteRing(audioEngine->getModSmoothingMs(targetStrip) / 250.0f)
                : makeBipolarRing(audioEngine->getModCurveBend(targetStrip));
            sendRingIfChanged(3, ring);
        }
    }
    else
    {
        if (hasStrip && strip->isPlaying())
            overlayStep = juce::jlimit(0, 15, strip->getCurrentColumn());

        if (grainMode)
        {
            if (ringCount >= 1)
                sendRingIfChanged(0, makeAbsoluteRing((strip->getGrainSizeMs() - 5.0f) / (2400.0f - 5.0f)));
            if (ringCount >= 2)
                sendRingIfChanged(1, makeAbsoluteRing(strip->getGrainDensity()));
            if (ringCount >= 3)
                sendRingIfChanged(2, makeBipolarRing(juce::jlimit(-1.0f, 1.0f, strip->getGrainPitch() / 24.0f)));
            if (ringCount >= 4)
                sendRingIfChanged(3, makeAbsoluteRing(strip->getGrainSpread()));
        }
        else
        {
            if (ringCount >= 1)
                sendRingIfChanged(0, makeAbsoluteRing(hasStrip ? (strip->getPlaybackSpeed() / 4.0f) : 0.0f));
            if (ringCount >= 2)
                sendRingIfChanged(1, makeBipolarRing(hasStrip ? (strip->getPitchShift() / 24.0f) : 0.0f));
            if (ringCount >= 3)
            {
                const float filterNorm = hasStrip ? normalizeFilterHz(strip->getFilterFrequency()) : 0.0f;
                sendRingIfChanged(2, makeAbsoluteRing(filterNorm));
            }
            if (ringCount >= 4)
                sendRingIfChanged(3, makeAbsoluteRing(audioEngine->getModDepth(targetStrip)));
        }
    }

    if (overlayStep >= 0 && overlayStep <= 15)
    {
        const int overlayIndex = stepToRingIndex(overlayStep);
        for (int encoder = 0; encoder < ringCount; ++encoder)
        {
            auto ring = arcRingCache[static_cast<size_t>(encoder)];
            overlayIndicator(ring, overlayIndex);
            monomeConnection.setArcRingMap(encoder, ring);
        }
    }

    // Tiny mode cue in the final 4 LEDs of ring 0.
    monomeConnection.setArcRingRange(0, 60, 63, modulationMode ? 15 : 5);
    monomeConnection.setArcRingLevel(0, 63, modulationMode ? 15 : 9);
}
