#include "TimeStretchBackend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#ifndef MLRVST_ENABLE_SOUNDTOUCH
#define MLRVST_ENABLE_SOUNDTOUCH 0
#endif

#ifndef MLRVST_ENABLE_BUNGEE
#define MLRVST_ENABLE_BUNGEE 0
#endif

#if MLRVST_ENABLE_SOUNDTOUCH
 #include <soundtouch/SoundTouch.h>
#endif

#if MLRVST_ENABLE_BUNGEE
 #include <bungee/Bungee.h>
#endif

namespace
{
constexpr float kMinStretchRatio = 0.25f;
constexpr float kMaxStretchRatio = 4.0f;
constexpr int kMinSoundTouchSafeFrames = 1024;

juce::AudioBuffer<float> buildStereoSourceBuffer(const juce::AudioBuffer<float>& sourceBuffer)
{
    const int sourceChannels = juce::jmax(1, sourceBuffer.getNumChannels());
    const int sourceFrames = juce::jmax(0, sourceBuffer.getNumSamples());
    juce::AudioBuffer<float> stereo(2, sourceFrames);
    stereo.clear();

    if (sourceFrames <= 0)
        return stereo;

    stereo.copyFrom(0, 0, sourceBuffer, 0, 0, sourceFrames);
    if (sourceChannels > 1)
        stereo.copyFrom(1, 0, sourceBuffer, 1, 0, sourceFrames);
    else
        stereo.copyFrom(1, 0, sourceBuffer, 0, 0, sourceFrames);

    return stereo;
}

void resampleBufferToLength(const juce::AudioBuffer<float>& sourceBuffer,
                            int targetFrameCount,
                            juce::AudioBuffer<float>& outputBuffer)
{
    const int sourceChannels = juce::jmax(1, sourceBuffer.getNumChannels());
    const int sourceFrames = juce::jmax(0, sourceBuffer.getNumSamples());
    const int targetFrames = juce::jmax(1, targetFrameCount);

    outputBuffer.setSize(sourceChannels, targetFrames, false, false, true);
    outputBuffer.clear();

    if (sourceFrames <= 0)
        return;

    if (sourceFrames == 1)
    {
        for (int ch = 0; ch < sourceChannels; ++ch)
            outputBuffer.clear(ch, 0, targetFrames);
        for (int ch = 0; ch < sourceChannels; ++ch)
        {
            const float value = sourceBuffer.getSample(ch, 0);
            outputBuffer.clear(ch, 0, targetFrames);
            outputBuffer.addFrom(ch, 0, sourceBuffer, ch, 0, 1, 1.0f);
            for (int frame = 0; frame < targetFrames; ++frame)
                outputBuffer.setSample(ch, frame, value);
        }
        return;
    }

    for (int ch = 0; ch < sourceChannels; ++ch)
    {
        const float* src = sourceBuffer.getReadPointer(ch);
        float* dst = outputBuffer.getWritePointer(ch);
        for (int frame = 0; frame < targetFrames; ++frame)
        {
            const double sourcePos = (targetFrames > 1)
                ? (static_cast<double>(frame) * static_cast<double>(sourceFrames - 1)
                   / static_cast<double>(targetFrames - 1))
                : 0.0;
            const int indexA = juce::jlimit(0, sourceFrames - 1, static_cast<int>(std::floor(sourcePos)));
            const int indexB = juce::jlimit(0, sourceFrames - 1, indexA + 1);
            const float frac = static_cast<float>(sourcePos - static_cast<double>(indexA));
            dst[frame] = src[indexA] + ((src[indexB] - src[indexA]) * frac);
        }
    }
}

#if MLRVST_ENABLE_SOUNDTOUCH
bool renderWithSoundTouch(const juce::AudioBuffer<float>& sourceBuffer,
                          double sourceSampleRate,
                          int targetFrameCount,
                          float pitchSemitones,
                          juce::AudioBuffer<float>& outputBuffer)
{
    const int sourceFrames = sourceBuffer.getNumSamples();
    const int targetFrames = juce::jmax(1, targetFrameCount);
    if (sourceFrames <= 0 || sourceSampleRate <= 0.0)
        return false;

    const auto stereoBuffer = buildStereoSourceBuffer(sourceBuffer);
    const float tempoRatio = juce::jlimit(kMinStretchRatio,
                                          kMaxStretchRatio,
                                          static_cast<float>(static_cast<double>(sourceFrames)
                                                             / static_cast<double>(targetFrames)));
    const bool isShortBuffer = sourceFrames < kMinSoundTouchSafeFrames
        || targetFrames < kMinSoundTouchSafeFrames;

    soundtouch::SoundTouch stretcher;
    stretcher.setSampleRate(static_cast<uint>(juce::jmax(1.0, sourceSampleRate)));
    stretcher.setChannels(2);
    stretcher.setTempo(tempoRatio);
    stretcher.setRate(1.0f);
    stretcher.setPitchSemiTones(pitchSemitones);
    stretcher.setSetting(SETTING_USE_AA_FILTER, isShortBuffer ? 0 : 1);
    stretcher.setSetting(SETTING_AA_FILTER_LENGTH, 32);
    stretcher.setSetting(SETTING_USE_QUICKSEEK, isShortBuffer ? 0 : 1);

    std::vector<float> interleaved(static_cast<size_t>(sourceFrames) * 2u, 0.0f);
    const float* left = stereoBuffer.getReadPointer(0);
    const float* right = stereoBuffer.getReadPointer(1);
    for (int frame = 0; frame < sourceFrames; ++frame)
    {
        const size_t base = static_cast<size_t>(frame) * 2u;
        interleaved[base] = left[frame];
        interleaved[base + 1u] = right[frame];
    }

    stretcher.putSamples(interleaved.data(), static_cast<uint>(sourceFrames));
    stretcher.flush();

    std::vector<float> stretchedInterleaved;
    stretchedInterleaved.reserve(static_cast<size_t>(targetFrames) * 4u);
    std::array<float, 4096> receiveScratch {};
    for (;;)
    {
        const uint frames = stretcher.receiveSamples(receiveScratch.data(),
                                                     static_cast<uint>(receiveScratch.size() / 2));
        if (frames == 0)
            break;

        const size_t samplesToAppend = static_cast<size_t>(frames) * 2u;
        stretchedInterleaved.insert(stretchedInterleaved.end(),
                                    receiveScratch.begin(),
                                    receiveScratch.begin() + static_cast<std::ptrdiff_t>(samplesToAppend));
    }

    if (stretchedInterleaved.empty())
        return false;

    const int stretchedFrames = static_cast<int>(stretchedInterleaved.size() / 2u);
    juce::AudioBuffer<float> rawOutput(2, stretchedFrames);
    for (int frame = 0; frame < stretchedFrames; ++frame)
    {
        const size_t base = static_cast<size_t>(frame) * 2u;
        rawOutput.setSample(0, frame, stretchedInterleaved[base]);
        rawOutput.setSample(1, frame, stretchedInterleaved[base + 1u]);
    }

    if (stretchedFrames == targetFrames)
    {
        outputBuffer = std::move(rawOutput);
        return true;
    }

    resampleBufferToLength(rawOutput, targetFrames, outputBuffer);
    return true;
}
#endif

#if MLRVST_ENABLE_BUNGEE
bool renderWithBungee(const juce::AudioBuffer<float>& sourceBuffer,
                      double sourceSampleRate,
                      int targetFrameCount,
                      float pitchSemitones,
                      juce::AudioBuffer<float>& outputBuffer)
{
    const int sourceFrames = sourceBuffer.getNumSamples();
    const int targetFrames = juce::jmax(1, targetFrameCount);
    if (sourceFrames <= 0 || sourceSampleRate <= 0.0)
        return false;

    const auto stereoBuffer = buildStereoSourceBuffer(sourceBuffer);
    const double speed = juce::jlimit(static_cast<double>(kMinStretchRatio),
                                      static_cast<double>(kMaxStretchRatio),
                                      static_cast<double>(sourceFrames)
                                          / static_cast<double>(targetFrames));
    const double pitchRatio = std::pow(2.0, static_cast<double>(pitchSemitones) / 12.0);

    Bungee::SampleRates sampleRates {
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate))),
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate)))
    };
    Bungee::Stretcher<Bungee::Basic> stretcher(sampleRates, 2);

    Bungee::Request request {};
    request.position = 0.0;
    request.speed = speed;
    request.pitch = pitchRatio;
    request.reset = false;
    request.resampleMode = resampleMode_autoOut;
    stretcher.preroll(request);

    const int maxInputFrameCount = juce::jmax(1, stretcher.maxInputFrameCount());
    std::vector<float> analysisScratch(static_cast<size_t>(maxInputFrameCount) * 2u, 0.0f);

    juce::AudioBuffer<float> rawOutput(2, targetFrames);
    rawOutput.clear();
    int writtenFrames = 0;
    int guard = 0;

    while (writtenFrames < targetFrames && guard++ < 20000)
    {
        const auto inputChunk = stretcher.specifyGrain(request);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* dst = analysisScratch.data() + (static_cast<size_t>(ch) * static_cast<size_t>(maxInputFrameCount));
            std::fill(dst, dst + maxInputFrameCount, 0.0f);
            const float* src = stereoBuffer.getReadPointer(ch);
            for (int sample = inputChunk.begin; sample < inputChunk.end; ++sample)
            {
                if (sample < 0 || sample >= sourceFrames)
                    continue;

                const int scratchIndex = sample - inputChunk.begin;
                if (scratchIndex >= 0 && scratchIndex < maxInputFrameCount)
                    dst[scratchIndex] = src[sample];
            }
        }

        stretcher.analyseGrain(analysisScratch.data(), maxInputFrameCount, 0, 0);

        Bungee::OutputChunk outputChunk {};
        stretcher.synthesiseGrain(outputChunk);
        stretcher.next(request);

        int trimFrames = 0;
        const auto* requestBegin = outputChunk.request[Bungee::OutputChunk::begin];
        const auto* requestEnd = outputChunk.request[Bungee::OutputChunk::end];
        if (requestBegin != nullptr
            && requestEnd != nullptr
            && !std::isnan(requestBegin->position)
            && std::abs(requestBegin->position - requestEnd->position) > 1.0e-9)
        {
            double prerollInputFrames = requestBegin->speed < 0.0
                ? requestBegin->position - static_cast<double>(sourceFrames) + 1.0
                : -requestBegin->position;
            prerollInputFrames = juce::jmax(0.0, std::round(prerollInputFrames));
            trimFrames = static_cast<int>(std::round(
                prerollInputFrames
                * (static_cast<double>(outputChunk.frameCount)
                   / std::abs(requestEnd->position - requestBegin->position))));
            trimFrames = juce::jlimit(0, outputChunk.frameCount, trimFrames);
        }

        for (int frame = trimFrames; frame < outputChunk.frameCount && writtenFrames < targetFrames; ++frame)
        {
            rawOutput.setSample(0, writtenFrames,
                                outputChunk.data[frame + (0 * outputChunk.channelStride)]);
            rawOutput.setSample(1, writtenFrames,
                                outputChunk.data[frame + (1 * outputChunk.channelStride)]);
            ++writtenFrames;
        }
    }

    if (writtenFrames <= 0)
        return false;

    if (writtenFrames == targetFrames)
    {
        outputBuffer = std::move(rawOutput);
        return true;
    }

    juce::AudioBuffer<float> trimmed(2, writtenFrames);
    trimmed.copyFrom(0, 0, rawOutput, 0, 0, writtenFrames);
    trimmed.copyFrom(1, 0, rawOutput, 1, 0, writtenFrames);
    resampleBufferToLength(trimmed, targetFrames, outputBuffer);
    return true;
}
#endif
} // namespace

TimeStretchBackend sanitizeTimeStretchBackend(int rawBackend) noexcept
{
    switch (rawBackend)
    {
        case static_cast<int>(TimeStretchBackend::Resample):
            return TimeStretchBackend::Resample;
        case static_cast<int>(TimeStretchBackend::SoundTouch):
            return TimeStretchBackend::SoundTouch;
        case static_cast<int>(TimeStretchBackend::Bungee):
            return TimeStretchBackend::Bungee;
        default:
            return TimeStretchBackend::Resample;
    }
}

const char* timeStretchBackendName(TimeStretchBackend backend) noexcept
{
    switch (backend)
    {
        case TimeStretchBackend::Resample:   return "Resample";
        case TimeStretchBackend::SoundTouch: return "SoundTouch";
        case TimeStretchBackend::Bungee:     return "Bungee";
        default:                             return "Resample";
    }
}

bool isTimeStretchBackendAvailable(TimeStretchBackend backend) noexcept
{
    switch (backend)
    {
        case TimeStretchBackend::Resample:
            return true;
        case TimeStretchBackend::SoundTouch:
#if MLRVST_ENABLE_SOUNDTOUCH
            return true;
#else
            return false;
#endif
        case TimeStretchBackend::Bungee:
#if MLRVST_ENABLE_BUNGEE
            return true;
#else
            return false;
#endif
        default:
            return false;
    }
}

bool renderTimeStretchedBuffer(const juce::AudioBuffer<float>& sourceBuffer,
                               double sourceSampleRate,
                               int targetFrameCount,
                               float pitchSemitones,
                               TimeStretchBackend backend,
                               juce::AudioBuffer<float>& outputBuffer)
{
    const auto sanitizedBackend = sanitizeTimeStretchBackend(static_cast<int>(backend));
    const int sourceFrames = sourceBuffer.getNumSamples();
    const int targetFrames = juce::jmax(1, targetFrameCount);
    if (sourceFrames <= 0 || sourceSampleRate <= 0.0)
        return false;

    switch (sanitizedBackend)
    {
        case TimeStretchBackend::Resample:
            resampleBufferToLength(buildStereoSourceBuffer(sourceBuffer), targetFrames, outputBuffer);
            return true;

        case TimeStretchBackend::SoundTouch:
#if MLRVST_ENABLE_SOUNDTOUCH
            if (sourceFrames < kMinSoundTouchSafeFrames || targetFrames < kMinSoundTouchSafeFrames)
            {
#if MLRVST_ENABLE_BUNGEE
                return renderWithBungee(sourceBuffer, sourceSampleRate, targetFrames, pitchSemitones, outputBuffer);
#else
                resampleBufferToLength(buildStereoSourceBuffer(sourceBuffer), targetFrames, outputBuffer);
                return true;
#endif
            }
            return renderWithSoundTouch(sourceBuffer, sourceSampleRate, targetFrames, pitchSemitones, outputBuffer);
#else
            return false;
#endif

        case TimeStretchBackend::Bungee:
#if MLRVST_ENABLE_BUNGEE
            return renderWithBungee(sourceBuffer, sourceSampleRate, targetFrames, pitchSemitones, outputBuffer);
#else
            return false;
#endif
        default:
            return false;
    }
}

bool renderTimeStretchedBufferForRate(const juce::AudioBuffer<float>& sourceBuffer,
                                      double sourceSampleRate,
                                      float playbackRate,
                                      float pitchSemitones,
                                      TimeStretchBackend backend,
                                      juce::AudioBuffer<float>& outputBuffer)
{
    const int sourceFrames = sourceBuffer.getNumSamples();
    if (sourceFrames <= 0)
        return false;

    const float safeRate = juce::jlimit(0.03125f, 8.0f, playbackRate);
    const int targetFrames = juce::jmax(1, static_cast<int>(std::lround(static_cast<double>(sourceFrames)
                                                                        / static_cast<double>(safeRate))));
    return renderTimeStretchedBuffer(sourceBuffer,
                                     sourceSampleRate,
                                     targetFrames,
                                     pitchSemitones,
                                     backend,
                                     outputBuffer);
}
