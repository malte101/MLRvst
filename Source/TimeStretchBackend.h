#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

enum class TimeStretchBackend
{
    Resample = 0,
    SoundTouch,
    Bungee
};

TimeStretchBackend sanitizeTimeStretchBackend(int rawBackend) noexcept;
const char* timeStretchBackendName(TimeStretchBackend backend) noexcept;
bool isTimeStretchBackendAvailable(TimeStretchBackend backend) noexcept;

bool renderTimeStretchedBuffer(const juce::AudioBuffer<float>& sourceBuffer,
                               double sourceSampleRate,
                               int targetFrameCount,
                               float pitchSemitones,
                               TimeStretchBackend backend,
                               juce::AudioBuffer<float>& outputBuffer);

bool renderTimeStretchedBufferForRate(const juce::AudioBuffer<float>& sourceBuffer,
                                      double sourceSampleRate,
                                      float playbackRate,
                                      float pitchSemitones,
                                      TimeStretchBackend backend,
                                      juce::AudioBuffer<float>& outputBuffer);
