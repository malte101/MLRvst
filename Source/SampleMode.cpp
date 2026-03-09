#include "SampleMode.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if MLRVST_ENABLE_SOUNDTOUCH
 #include <soundtouch/SoundTouch.h>
#endif

namespace
{
constexpr double kInitialSliceTargetSeconds = 1.5;
constexpr int kMaxInitialSliceCount = 256;
constexpr int kDefaultSliceFadeSamples = 96;
constexpr int kDefaultAnalysisHop = 512;
constexpr int kDefaultAnalysisWindow = 2048;
constexpr int kMinTransientGapSamples = 2048;
constexpr int kMaxStoredTransientCount = 512;
constexpr float kMinViewZoom = 1.0f;
constexpr float kMaxViewZoom = 32.0f;
constexpr float kCueHitRadiusPixels = 8.0f;
constexpr const char* kAudioFluxScriptName = "mlrvst_flip_audioflux_analysis.py";
constexpr float kKeyLockCacheRateTolerance = 0.0005f;
constexpr float kKeyLockCachePitchTolerance = 0.01f;

juce::String buildSliceLabel(int index);

const char* sampleSliceModeName(SampleSliceMode mode)
{
    switch (mode)
    {
        case SampleSliceMode::Uniform: return "uniform";
        case SampleSliceMode::Transient: return "transient";
        case SampleSliceMode::Beat: return "beat";
        case SampleSliceMode::Manual: return "manual";
    }

    return "uniform";
}

SampleSliceMode sampleSliceModeFromString(const juce::String& text)
{
    const auto normalized = text.trim().toLowerCase();
    if (normalized == "transient")
        return SampleSliceMode::Transient;
    if (normalized == "beat")
        return SampleSliceMode::Beat;
    if (normalized == "manual")
        return SampleSliceMode::Manual;
    return SampleSliceMode::Uniform;
}

const char* sampleTriggerModeName(SampleTriggerMode mode)
{
    switch (mode)
    {
        case SampleTriggerMode::OneShot: return "oneshot";
        case SampleTriggerMode::Loop: return "loop";
    }

    return "oneshot";
}

SampleTriggerMode sampleTriggerModeFromString(const juce::String& text)
{
    return text.trim().equalsIgnoreCase("loop")
        ? SampleTriggerMode::Loop
        : SampleTriggerMode::OneShot;
}

juce::String noteNameForMidi(int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (midiNote < 0)
        return {};

    const int pitchClass = ((midiNote % 12) + 12) % 12;
    const int octave = (midiNote / 12) - 1;
    return juce::String(names[pitchClass]) + juce::String(octave);
}

float safeNormalizedPosition(int64_t samplePosition, int64_t totalSamples)
{
    if (totalSamples <= 0)
        return 0.0f;

    return juce::jlimit(0.0f,
                        1.0f,
                        static_cast<float>(samplePosition / static_cast<double>(juce::jmax<int64_t>(1, totalSamples))));
}

juce::AudioBuffer<float> buildMonoBuffer(const LoadedSampleData& sampleData)
{
    juce::AudioBuffer<float> monoBuffer(1, sampleData.audioBuffer.getNumSamples());
    monoBuffer.clear();

    const int sourceChannels = sampleData.audioBuffer.getNumChannels();
    const int numSamples = sampleData.audioBuffer.getNumSamples();
    if (sourceChannels <= 0 || numSamples <= 0)
        return monoBuffer;

    float* mono = monoBuffer.getWritePointer(0);
    if (sourceChannels == 1)
    {
        juce::FloatVectorOperations::copy(mono, sampleData.audioBuffer.getReadPointer(0), numSamples);
        return monoBuffer;
    }

    const float* left = sampleData.audioBuffer.getReadPointer(0);
    const float* right = sampleData.audioBuffer.getReadPointer(1);
    for (int i = 0; i < numSamples; ++i)
        mono[i] = 0.5f * (left[i] + right[i]);

    return monoBuffer;
}

std::vector<float> computeFrameEnergy(const juce::AudioBuffer<float>& monoBuffer,
                                      int windowSize,
                                      int hopSize)
{
    std::vector<float> energy;
    const int numSamples = monoBuffer.getNumSamples();
    if (numSamples <= 0 || windowSize <= 0 || hopSize <= 0)
        return energy;

    const float* mono = monoBuffer.getReadPointer(0);
    for (int start = 0; start < numSamples; start += hopSize)
    {
        const int end = juce::jmin(numSamples, start + windowSize);
        if (end <= start)
            break;

        double sumSquares = 0.0;
        for (int i = start; i < end; ++i)
        {
            const double sample = mono[i];
            sumSquares += sample * sample;
        }

        energy.push_back(static_cast<float>(std::sqrt(sumSquares / static_cast<double>(end - start))));
    }

    return energy;
}

std::vector<int64_t> detectTransientSamplesFromEnergy(const std::vector<float>& energy,
                                                      int hopSize,
                                                      double sampleRate)
{
    std::vector<int64_t> transientSamples;
    if (energy.size() < 3 || hopSize <= 0 || sampleRate <= 0.0)
        return transientSamples;

    std::vector<float> flux(energy.size(), 0.0f);
    double sum = 0.0;
    for (size_t i = 1; i < energy.size(); ++i)
    {
        const float delta = juce::jmax(0.0f, energy[i] - energy[i - 1]);
        flux[i] = delta;
        sum += delta;
    }

    const float mean = static_cast<float>(sum / static_cast<double>(juce::jmax<size_t>(1, energy.size() - 1)));
    float variance = 0.0f;
    for (float value : flux)
    {
        const float diff = value - mean;
        variance += diff * diff;
    }
    variance /= static_cast<float>(juce::jmax<size_t>(1, flux.size()));
    const float threshold = mean + (0.65f * std::sqrt(variance));

    const int minGapFrames = juce::jmax(1,
        static_cast<int>(std::round((0.08 * sampleRate) / static_cast<double>(hopSize))));
    int lastAcceptedFrame = -minGapFrames;

    for (size_t i = 1; i + 1 < flux.size(); ++i)
    {
        const float value = flux[i];
        if (value < threshold)
            continue;
        if (value < flux[i - 1] || value < flux[i + 1])
            continue;

        const int frameIndex = static_cast<int>(i);
        if ((frameIndex - lastAcceptedFrame) < minGapFrames)
            continue;

        transientSamples.push_back(static_cast<int64_t>(frameIndex) * hopSize);
        lastAcceptedFrame = frameIndex;

        if (static_cast<int>(transientSamples.size()) >= kMaxStoredTransientCount)
            break;
    }

    return transientSamples;
}

double normalizeTempoEstimate(double bpm)
{
    if (!(bpm > 0.0) || !std::isfinite(bpm))
        return 0.0;

    while (bpm < 70.0)
        bpm *= 2.0;
    while (bpm > 180.0)
        bpm *= 0.5;
    return bpm;
}

double estimateTempoFromTransients(const std::vector<int64_t>& transientSamples, double sampleRate)
{
    if (transientSamples.size() < 2 || sampleRate <= 0.0)
        return 0.0;

    std::vector<double> intervalsSeconds;
    intervalsSeconds.reserve(transientSamples.size() - 1);
    for (size_t i = 1; i < transientSamples.size(); ++i)
    {
        const double deltaSamples = static_cast<double>(transientSamples[i] - transientSamples[i - 1]);
        const double seconds = deltaSamples / sampleRate;
        if (seconds > 0.08 && seconds < 2.0)
            intervalsSeconds.push_back(seconds);
    }

    if (intervalsSeconds.empty())
        return 0.0;

    std::nth_element(intervalsSeconds.begin(),
                     intervalsSeconds.begin() + static_cast<std::ptrdiff_t>(intervalsSeconds.size() / 2),
                     intervalsSeconds.end());
    const double medianSeconds = intervalsSeconds[intervalsSeconds.size() / 2];
    if (!(medianSeconds > 0.0))
        return 0.0;

    return normalizeTempoEstimate(60.0 / medianSeconds);
}

double estimatePitchHzFromAutocorrelation(const juce::AudioBuffer<float>& monoBuffer,
                                          double sampleRate)
{
    if (sampleRate <= 0.0 || monoBuffer.getNumSamples() < 2048)
        return 0.0;

    const float* mono = monoBuffer.getReadPointer(0);
    const int numSamples = monoBuffer.getNumSamples();
    const int windowSize = juce::jmin(8192, juce::jmax(2048, numSamples / 4));
    const int maxStart = juce::jmax(0, numSamples - windowSize);
    int bestStart = 0;
    double bestEnergy = 0.0;

    for (int start = 0; start <= maxStart; start += juce::jmax(256, windowSize / 4))
    {
        double energy = 0.0;
        for (int i = start; i < start + windowSize; ++i)
        {
            const double sample = mono[i];
            energy += sample * sample;
        }

        if (energy > bestEnergy)
        {
            bestEnergy = energy;
            bestStart = start;
        }
    }

    if (bestEnergy <= 1.0e-6)
        return 0.0;

    const int minLag = juce::jmax(8, static_cast<int>(std::floor(sampleRate / 1000.0)));
    const int maxLag = juce::jmin(windowSize / 2, static_cast<int>(std::ceil(sampleRate / 50.0)));
    double bestCorrelation = 0.0;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double correlation = 0.0;
        for (int i = 0; i < (windowSize - lag); ++i)
            correlation += static_cast<double>(mono[bestStart + i]) * mono[bestStart + i + lag];

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || bestCorrelation <= 1.0e-6)
        return 0.0;

    return sampleRate / static_cast<double>(bestLag);
}

int pitchHzToMidi(double pitchHz)
{
    if (!(pitchHz > 0.0) || !std::isfinite(pitchHz))
        return -1;

    return static_cast<int>(std::lround(69.0 + (12.0 * std::log2(pitchHz / 440.0))));
}

juce::String buildCueName(int index)
{
    return "C" + juce::String(index + 1);
}

float quantizeKeyLockRate(float playbackRate)
{
    return std::round(juce::jlimit(0.03125f, 8.0f, playbackRate) * 1000.0f) / 1000.0f;
}

float quantizeKeyLockPitch(float pitchSemitones)
{
    return std::round(juce::jlimit(-24.0f, 24.0f, pitchSemitones) * 100.0f) / 100.0f;
}

bool matchesKeyLockRequest(const StretchedSliceBuffer& cache,
                           const SampleSlice& slice,
                           float playbackRate,
                           float pitchSemitones)
{
    return cache.sliceId == slice.id
        && cache.sliceStartSample == slice.startSample
        && cache.sliceEndSample == slice.endSample
        && std::abs(cache.playbackRate - quantizeKeyLockRate(playbackRate)) <= kKeyLockCacheRateTolerance
        && std::abs(cache.pitchSemitones - quantizeKeyLockPitch(pitchSemitones)) <= kKeyLockCachePitchTolerance;
}

juce::String buildAudioFluxScript()
{
    return juce::String(
R"PY(import json
import math
import os
import sys
import tempfile

os.environ.setdefault("MPLCONFIGDIR", os.path.join(tempfile.gettempdir(), "mlrvst_audioflux_mpl"))
os.environ.setdefault("XDG_CACHE_HOME", tempfile.gettempdir())

try:
    import numpy as np
    import audioflux as af
except Exception as exc:
    print(json.dumps({"ok": False, "error": str(exc)}))
    raise SystemExit(0)

path = sys.argv[1]

try:
    audio_arr, sr = af.read(path)
    audio_arr = np.asarray(audio_arr, dtype=np.float32)
    if audio_arr.ndim > 1:
        audio_arr = np.mean(audio_arr, axis=0)

    frame_length = 2048
    slide_length = 512
    temporal = af.Temporal(frame_length=frame_length, slide_length=slide_length)
    features = temporal.temporal(audio_arr, has_energy=True, has_rms=True, has_zcr=True)
    energy_arr = None
    if isinstance(features, dict):
        energy_arr = features.get("energy_arr", features.get("energy", None))
    if energy_arr is None:
        energy_arr = np.abs(audio_arr)
    energy_arr = np.asarray(energy_arr, dtype=np.float32).reshape(-1)
    if energy_arr.size < 3:
        energy_arr = np.abs(audio_arr[::slide_length]).astype(np.float32)

    flux = np.maximum(0.0, np.diff(energy_arr, prepend=energy_arr[0]))
    threshold = float(np.mean(flux) + (0.65 * np.std(flux)))
    onset_frames = []
    min_gap = max(1, int((0.08 * sr) / slide_length))
    last_idx = -min_gap
    for i in range(1, flux.shape[0] - 1):
        value = float(flux[i])
        if value < threshold or value < float(flux[i - 1]) or value < float(flux[i + 1]):
            continue
        if (i - last_idx) < min_gap:
            continue
        onset_frames.append(i)
        last_idx = i
        if len(onset_frames) >= 512:
            break
    transient_samples = [int(i * slide_length) for i in onset_frames]

    tempo = 0.0
    if len(transient_samples) >= 2:
        intervals = []
        for i in range(1, len(transient_samples)):
            delta = (transient_samples[i] - transient_samples[i - 1]) / float(sr)
            if 0.08 < delta < 2.0:
                intervals.append(delta)
        if intervals:
            median = float(np.median(np.asarray(intervals, dtype=np.float32)))
            if median > 0.0:
                tempo = 60.0 / median
                while tempo < 70.0:
                    tempo *= 2.0
                while tempo > 180.0:
                    tempo *= 0.5

    pitch_hz = 0.0
    try:
        pitch = af.PitchYIN(samplate=sr, slide_length=slide_length)
        pitch_arr = pitch.pitch(audio_arr)
        if isinstance(pitch_arr, tuple) or isinstance(pitch_arr, list):
            pitch_arr = pitch_arr[0]
        pitch_arr = np.asarray(pitch_arr, dtype=np.float32).reshape(-1)
        pitch_arr = pitch_arr[(pitch_arr > 20.0) & (pitch_arr < 5000.0)]
        if pitch_arr.size > 0:
            pitch_hz = float(np.median(pitch_arr))
    except Exception:
        pitch_hz = 0.0

    pitch_midi = -1
    if pitch_hz > 0.0:
        pitch_midi = int(round(69.0 + (12.0 * math.log(pitch_hz / 440.0, 2.0))))

    print(json.dumps({
        "ok": True,
        "tempo_bpm": tempo,
        "pitch_hz": pitch_hz,
        "pitch_midi": pitch_midi,
        "transient_samples": transient_samples,
        "analysis_source": "audioflux"
    }))
except Exception as exc:
    print(json.dumps({"ok": False, "error": str(exc)}))
)PY");
}

juce::File ensureAudioFluxScriptFile()
{
    const auto scriptFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile(kAudioFluxScriptName);
    const auto scriptText = buildAudioFluxScript();

    if (!scriptFile.existsAsFile() || scriptFile.loadFileAsString() != scriptText)
        scriptFile.replaceWithText(scriptText);

    return scriptFile;
}

std::optional<SampleAnalysisSummary> runAudioFluxAnalysis(const juce::File& sourceFile)
{
    if (!sourceFile.existsAsFile())
        return std::nullopt;

    const auto scriptFile = ensureAudioFluxScriptFile();
    if (!scriptFile.existsAsFile())
        return std::nullopt;

    juce::ChildProcess childProcess;
    juce::StringArray args;
    args.add("python3");
    args.add(scriptFile.getFullPathName());
    args.add(sourceFile.getFullPathName());

    if (!childProcess.start(args))
        return std::nullopt;

    if (!childProcess.waitForProcessToFinish(30000))
    {
        childProcess.kill();
        return std::nullopt;
    }

    const auto output = childProcess.readAllProcessOutput().trim();
    if (output.isEmpty())
        return std::nullopt;

    const auto parsed = juce::JSON::parse(output);
    if (!parsed.isObject())
        return std::nullopt;

    auto* object = parsed.getDynamicObject();
    if (object == nullptr || !object->getProperty("ok"))
        return std::nullopt;

    SampleAnalysisSummary summary;
    summary.estimatedTempoBpm = normalizeTempoEstimate(static_cast<double>(object->getProperty("tempo_bpm")));
    summary.estimatedPitchHz = static_cast<double>(object->getProperty("pitch_hz"));
    summary.estimatedPitchMidi = static_cast<int>(object->getProperty("pitch_midi"));
    summary.audioFluxUsed = true;
    summary.analysisSource = object->getProperty("analysis_source").toString();

    const auto transientVar = object->getProperty("transient_samples");
    if (auto* transientArray = transientVar.getArray())
    {
        summary.transientSamples.reserve(static_cast<size_t>(transientArray->size()));
        for (const auto& item : *transientArray)
            summary.transientSamples.push_back(static_cast<int64_t>(item));
    }

    return summary;
}

SampleAnalysisSummary buildInternalAnalysis(const LoadedSampleData& sampleData)
{
    SampleAnalysisSummary summary;
    summary.analysisSource = "internal";

    const auto monoBuffer = buildMonoBuffer(sampleData);
    const auto energy = computeFrameEnergy(monoBuffer, kDefaultAnalysisWindow, kDefaultAnalysisHop);
    summary.transientSamples = detectTransientSamplesFromEnergy(energy,
                                                                kDefaultAnalysisHop,
                                                                sampleData.sourceSampleRate);
    summary.estimatedTempoBpm = estimateTempoFromTransients(summary.transientSamples,
                                                            sampleData.sourceSampleRate);
    summary.estimatedPitchHz = estimatePitchHzFromAutocorrelation(monoBuffer,
                                                                  sampleData.sourceSampleRate);
    summary.estimatedPitchMidi = pitchHzToMidi(summary.estimatedPitchHz);
    return summary;
}

void fillSliceMetadata(SampleSlice& slice, int sliceIndex, int64_t totalSamples)
{
    slice.id = sliceIndex;
    slice.startSample = juce::jmax<int64_t>(0, slice.startSample);
    slice.endSample = juce::jmax(slice.startSample + 1, slice.endSample);
    slice.normalizedStart = safeNormalizedPosition(slice.startSample, totalSamples);
    slice.normalizedEnd = safeNormalizedPosition(slice.endSample, totalSamples);
    slice.label = buildSliceLabel(sliceIndex);
}

float readInterpolatedSample(const juce::AudioBuffer<float>& buffer, int channel, double samplePosition)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return 0.0f;

    const int clampedChannel = juce::jlimit(0, numChannels - 1, channel);
    const float* data = buffer.getReadPointer(clampedChannel);
    const double clampedPos = juce::jlimit(0.0, static_cast<double>(juce::jmax(0, numSamples - 1)), samplePosition);
    const int indexA = static_cast<int>(clampedPos);
    const int indexB = juce::jmin(indexA + 1, numSamples - 1);
    const float frac = static_cast<float>(clampedPos - static_cast<double>(indexA));
    return juce::jmap(frac, data[indexA], data[indexB]);
}

void buildWaveformPreview(LoadedSampleData& sampleData)
{
    const auto numSamples = sampleData.audioBuffer.getNumSamples();
    const auto numChannels = sampleData.audioBuffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
    {
        sampleData.previewMin.clear();
        sampleData.previewMax.clear();
        return;
    }

    const int pointCount = juce::jmin(LoadedSampleData::PreviewPointCount, juce::jmax(64, numSamples));
    sampleData.previewMin.assign(static_cast<size_t>(pointCount), 0.0f);
    sampleData.previewMax.assign(static_cast<size_t>(pointCount), 0.0f);

    for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
    {
        const int startSample = static_cast<int>((static_cast<int64_t>(pointIndex) * numSamples) / pointCount);
        const int endSample = juce::jmax(startSample + 1,
                                         static_cast<int>((static_cast<int64_t>(pointIndex + 1) * numSamples) / pointCount));

        float minValue = 1.0f;
        float maxValue = -1.0f;

        for (int sampleIndex = startSample; sampleIndex < juce::jmin(endSample, numSamples); ++sampleIndex)
        {
            float monoSample = 0.0f;

            if (numChannels == 1)
                monoSample = sampleData.audioBuffer.getSample(0, sampleIndex);
            else
                monoSample = 0.5f * (sampleData.audioBuffer.getSample(0, sampleIndex)
                                   + sampleData.audioBuffer.getSample(1, sampleIndex));

            minValue = juce::jmin(minValue, monoSample);
            maxValue = juce::jmax(maxValue, monoSample);
        }

        if (minValue > maxValue)
        {
            minValue = 0.0f;
            maxValue = 0.0f;
        }

        sampleData.previewMin[static_cast<size_t>(pointIndex)] = minValue;
        sampleData.previewMax[static_cast<size_t>(pointIndex)] = maxValue;
    }
}

std::shared_ptr<const LoadedSampleData> buildLoadedSampleFromBuffer(const juce::AudioBuffer<float>& buffer,
                                                                    double sourceSampleRate,
                                                                    const juce::String& sourcePath,
                                                                    const juce::String& displayName)
{
    if (buffer.getNumSamples() <= 0
        || buffer.getNumChannels() <= 0
        || !std::isfinite(sourceSampleRate)
        || sourceSampleRate <= 1000.0)
    {
        return {};
    }

    auto loadedSample = std::make_shared<LoadedSampleData>();
    const int channelCount = juce::jlimit(1, 2, buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    loadedSample->audioBuffer.setSize(2, numSamples, false, false, true);
    loadedSample->audioBuffer.clear();

    loadedSample->audioBuffer.copyFrom(0, 0, buffer, 0, 0, numSamples);
    if (channelCount == 1)
        loadedSample->audioBuffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    else
        loadedSample->audioBuffer.copyFrom(1, 0, buffer, 1, 0, numSamples);

    loadedSample->sourcePath = sourcePath;
    loadedSample->displayName = displayName.isNotEmpty()
        ? displayName
        : (sourcePath.isNotEmpty()
            ? juce::File(sourcePath).getFileNameWithoutExtension()
            : juce::String("Embedded Flip Sample"));
    loadedSample->sourceSampleRate = sourceSampleRate;
    loadedSample->numChannels = channelCount;
    loadedSample->sourceLengthSamples = numSamples;
    loadedSample->durationSeconds = numSamples / juce::jmax(1.0, sourceSampleRate);
    buildWaveformPreview(*loadedSample);
    loadedSample->analysis = buildInternalAnalysis(*loadedSample);
    return std::const_pointer_cast<const LoadedSampleData>(loadedSample);
}

#if MLRVST_ENABLE_SOUNDTOUCH
std::shared_ptr<const StretchedSliceBuffer> buildStretchedSliceBuffer(const LoadedSampleData& sampleData,
                                                                      const SampleSlice& slice,
                                                                      float playbackRate,
                                                                      float pitchSemitones)
{
    const int64_t sliceStart = juce::jmax<int64_t>(0, slice.startSample);
    const int64_t sliceEnd = juce::jmin<int64_t>(sampleData.sourceLengthSamples,
                                                 juce::jmax(slice.startSample + 1, slice.endSample));
    const int64_t sliceLength64 = sliceEnd - sliceStart;
    if (sliceLength64 <= 0 || sliceLength64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return {};

    const int sliceLength = static_cast<int>(sliceLength64);
    juce::AudioBuffer<float> sourceSlice(2, sliceLength);
    sourceSlice.copyFrom(0, 0, sampleData.audioBuffer, 0, static_cast<int>(sliceStart), sliceLength);
    sourceSlice.copyFrom(1, 0, sampleData.audioBuffer, 1, static_cast<int>(sliceStart), sliceLength);

    const float quantizedRate = quantizeKeyLockRate(playbackRate);
    const float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);

    soundtouch::SoundTouch stretcher;
    stretcher.setSampleRate(static_cast<uint>(juce::roundToInt(sampleData.sourceSampleRate)));
    stretcher.setChannels(2);
    stretcher.setRate(1.0f);
    stretcher.setTempo(juce::jlimit(0.25f, 4.0f, quantizedRate));
    stretcher.setPitchSemiTones(quantizedPitch);
    stretcher.setSetting(SETTING_USE_AA_FILTER, 1);
    stretcher.setSetting(SETTING_AA_FILTER_LENGTH, 32);
    stretcher.setSetting(SETTING_USE_QUICKSEEK, 1);

    std::vector<float> interleaved(static_cast<size_t>(sliceLength) * 2u, 0.0f);
    const float* left = sourceSlice.getReadPointer(0);
    const float* right = sourceSlice.getReadPointer(1);
    for (int i = 0; i < sliceLength; ++i)
    {
        const size_t base = static_cast<size_t>(i) * 2u;
        interleaved[base] = left[i];
        interleaved[base + 1u] = right[i];
    }

    stretcher.putSamples(interleaved.data(), static_cast<uint>(sliceLength));
    stretcher.flush();

    std::vector<float> stretchedInterleaved;
    stretchedInterleaved.reserve(interleaved.size() * 2u);
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
        return {};

    const int outputFrames = static_cast<int>(stretchedInterleaved.size() / 2u);
    auto cache = std::make_shared<StretchedSliceBuffer>();
    cache->sliceId = slice.id;
    cache->sliceStartSample = slice.startSample;
    cache->sliceEndSample = slice.endSample;
    cache->playbackRate = quantizedRate;
    cache->pitchSemitones = quantizedPitch;
    cache->sourceSampleRate = sampleData.sourceSampleRate;
    cache->audioBuffer.setSize(2, outputFrames, false, false, true);

    float* cacheLeft = cache->audioBuffer.getWritePointer(0);
    float* cacheRight = cache->audioBuffer.getWritePointer(1);
    for (int frame = 0; frame < outputFrames; ++frame)
    {
        const size_t base = static_cast<size_t>(frame) * 2u;
        cacheLeft[frame] = stretchedInterleaved[base];
        cacheRight[frame] = stretchedInterleaved[base + 1u];
    }

    return std::const_pointer_cast<const StretchedSliceBuffer>(cache);
}
#endif

juce::String buildSliceLabel(int index)
{
    return "S" + juce::String(index + 1);
}
} // namespace

juce::ValueTree SampleModePersistentState::createValueTree(const juce::Identifier& type) const
{
    juce::ValueTree tree(type);
    tree.setProperty("samplePath", samplePath, nullptr);
    tree.setProperty("visibleSliceBankIndex", visibleSliceBankIndex, nullptr);
    tree.setProperty("sliceMode", sampleSliceModeName(sliceMode), nullptr);
    tree.setProperty("triggerMode", sampleTriggerModeName(triggerMode), nullptr);
    tree.setProperty("beatDivision", beatDivision, nullptr);
    tree.setProperty("viewZoom", viewZoom, nullptr);
    tree.setProperty("viewScroll", viewScroll, nullptr);
    tree.setProperty("selectedCueIndex", selectedCueIndex, nullptr);
    tree.setProperty("analyzedTempoBpm", analyzedTempoBpm, nullptr);
    tree.setProperty("analyzedPitchHz", analyzedPitchHz, nullptr);
    tree.setProperty("analyzedPitchMidi", analyzedPitchMidi, nullptr);
    tree.setProperty("analysisSource", analysisSource, nullptr);

    juce::ValueTree cuesNode("CuePoints");
    for (const auto& cue : cuePoints)
    {
        juce::ValueTree cueNode("Cue");
        cueNode.setProperty("id", cue.id, nullptr);
        cueNode.setProperty("samplePosition", static_cast<juce::int64>(cue.samplePosition), nullptr);
        cueNode.setProperty("name", cue.name, nullptr);
        cueNode.setProperty("loopEnabled", cue.loopEnabled, nullptr);
        cueNode.setProperty("loopEndSample", static_cast<juce::int64>(cue.loopEndSample), nullptr);
        cuesNode.addChild(cueNode, -1, nullptr);
    }
    tree.addChild(cuesNode, -1, nullptr);

    juce::ValueTree slicesNode("Slices");
    for (const auto& slice : storedSlices)
    {
        juce::ValueTree sliceNode("Slice");
        sliceNode.setProperty("id", slice.id, nullptr);
        sliceNode.setProperty("startSample", static_cast<juce::int64>(slice.startSample), nullptr);
        sliceNode.setProperty("endSample", static_cast<juce::int64>(slice.endSample), nullptr);
        sliceNode.setProperty("label", slice.label, nullptr);
        sliceNode.setProperty("transientDerived", slice.transientDerived, nullptr);
        sliceNode.setProperty("manualDerived", slice.manualDerived, nullptr);
        slicesNode.addChild(sliceNode, -1, nullptr);
    }
    tree.addChild(slicesNode, -1, nullptr);

    return tree;
}

SampleModePersistentState SampleModePersistentState::fromValueTree(const juce::ValueTree& state)
{
    SampleModePersistentState persistentState;
    if (!state.isValid())
        return persistentState;

    persistentState.samplePath = state.getProperty("samplePath").toString();
    persistentState.visibleSliceBankIndex = juce::jmax(0, static_cast<int>(state.getProperty("visibleSliceBankIndex", 0)));
    persistentState.sliceMode = sampleSliceModeFromString(state.getProperty("sliceMode", "uniform").toString());
    persistentState.triggerMode = sampleTriggerModeFromString(state.getProperty("triggerMode", "oneshot").toString());
    persistentState.beatDivision = juce::jlimit(1, 8, static_cast<int>(state.getProperty("beatDivision", 1)));
    persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, static_cast<float>(state.getProperty("viewZoom", 1.0f)));
    persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, static_cast<float>(state.getProperty("viewScroll", 0.0f)));
    persistentState.selectedCueIndex = static_cast<int>(state.getProperty("selectedCueIndex", -1));
    persistentState.analyzedTempoBpm = static_cast<double>(state.getProperty("analyzedTempoBpm", 0.0));
    persistentState.analyzedPitchHz = static_cast<double>(state.getProperty("analyzedPitchHz", 0.0));
    persistentState.analyzedPitchMidi = static_cast<int>(state.getProperty("analyzedPitchMidi", -1));
    persistentState.analysisSource = state.getProperty("analysisSource").toString();

    if (auto cuesNode = state.getChildWithName("CuePoints"); cuesNode.isValid())
    {
        for (auto cueNode : cuesNode)
        {
            if (!cueNode.hasType("Cue"))
                continue;

            SampleCuePoint cue;
            cue.id = static_cast<int>(cueNode.getProperty("id", -1));
            cue.samplePosition = static_cast<int64_t>(static_cast<juce::int64>(cueNode.getProperty("samplePosition", 0)));
            cue.name = cueNode.getProperty("name").toString();
            cue.loopEnabled = static_cast<bool>(cueNode.getProperty("loopEnabled", false));
            cue.loopEndSample = static_cast<int64_t>(static_cast<juce::int64>(cueNode.getProperty("loopEndSample", cue.samplePosition)));
            persistentState.cuePoints.push_back(std::move(cue));
        }
    }

    if (auto slicesNode = state.getChildWithName("Slices"); slicesNode.isValid())
    {
        for (auto sliceNode : slicesNode)
        {
            if (!sliceNode.hasType("Slice"))
                continue;

            SampleSlice slice;
            slice.id = static_cast<int>(sliceNode.getProperty("id", -1));
            slice.startSample = static_cast<int64_t>(static_cast<juce::int64>(sliceNode.getProperty("startSample", 0)));
            slice.endSample = static_cast<int64_t>(static_cast<juce::int64>(sliceNode.getProperty("endSample", 0)));
            slice.label = sliceNode.getProperty("label").toString();
            slice.transientDerived = static_cast<bool>(sliceNode.getProperty("transientDerived", false));
            slice.manualDerived = static_cast<bool>(sliceNode.getProperty("manualDerived", false));
            persistentState.storedSlices.push_back(std::move(slice));
        }
    }

    return persistentState;
}

std::unique_ptr<juce::XmlElement> SampleModePersistentState::createXml(const juce::String& tagName) const
{
    auto xml = std::make_unique<juce::XmlElement>(tagName);
    xml->setAttribute("samplePath", samplePath);
    xml->setAttribute("visibleSliceBankIndex", visibleSliceBankIndex);
    xml->setAttribute("sliceMode", sampleSliceModeName(sliceMode));
    xml->setAttribute("triggerMode", sampleTriggerModeName(triggerMode));
    xml->setAttribute("beatDivision", beatDivision);
    xml->setAttribute("viewZoom", viewZoom);
    xml->setAttribute("viewScroll", viewScroll);
    xml->setAttribute("selectedCueIndex", selectedCueIndex);
    xml->setAttribute("analyzedTempoBpm", analyzedTempoBpm);
    xml->setAttribute("analyzedPitchHz", analyzedPitchHz);
    xml->setAttribute("analyzedPitchMidi", analyzedPitchMidi);
    xml->setAttribute("analysisSource", analysisSource);

    for (const auto& cue : cuePoints)
    {
        auto* cueXml = xml->createNewChildElement("Cue");
        cueXml->setAttribute("id", cue.id);
        cueXml->setAttribute("samplePosition", juce::String(static_cast<juce::int64>(cue.samplePosition)));
        cueXml->setAttribute("name", cue.name);
        cueXml->setAttribute("loopEnabled", cue.loopEnabled);
        cueXml->setAttribute("loopEndSample", juce::String(static_cast<juce::int64>(cue.loopEndSample)));
    }

    for (const auto& slice : storedSlices)
    {
        auto* sliceXml = xml->createNewChildElement("Slice");
        sliceXml->setAttribute("id", slice.id);
        sliceXml->setAttribute("startSample", juce::String(static_cast<juce::int64>(slice.startSample)));
        sliceXml->setAttribute("endSample", juce::String(static_cast<juce::int64>(slice.endSample)));
        sliceXml->setAttribute("label", slice.label);
        sliceXml->setAttribute("transientDerived", slice.transientDerived);
        sliceXml->setAttribute("manualDerived", slice.manualDerived);
    }

    return xml;
}

SampleModePersistentState SampleModePersistentState::fromXml(const juce::XmlElement& xml)
{
    return fromValueTree(juce::ValueTree::fromXml(xml));
}

void SliceModel::clear()
{
    slices.clear();
    visibleBankIndex = 0;
}

void SliceModel::setSlices(std::vector<SampleSlice> newSlices)
{
    slices = std::move(newSlices);
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), visibleBankIndex);
}

void SliceModel::setVisibleBankIndex(int bankIndex)
{
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), bankIndex);
}

int SliceModel::getVisibleBankCount() const noexcept
{
    if (slices.empty())
        return 0;

    return (static_cast<int>(slices.size()) + VisibleSliceCount - 1) / VisibleSliceCount;
}

bool SliceModel::canNavigateLeft() const noexcept
{
    return visibleBankIndex > 0;
}

bool SliceModel::canNavigateRight() const noexcept
{
    return visibleBankIndex + 1 < getVisibleBankCount();
}

void SliceModel::stepVisibleBank(int delta)
{
    setVisibleBankIndex(visibleBankIndex + delta);
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SliceModel::getVisibleSlices() const
{
    std::array<SampleSlice, VisibleSliceCount> visibleSlices {};

    const int startIndex = visibleBankIndex * VisibleSliceCount;
    for (int slot = 0; slot < VisibleSliceCount; ++slot)
    {
        const int sliceIndex = startIndex + slot;
        if (sliceIndex >= 0 && sliceIndex < static_cast<int>(slices.size()))
            visibleSlices[static_cast<size_t>(slot)] = slices[static_cast<size_t>(sliceIndex)];
        else
            visibleSlices[static_cast<size_t>(slot)].label = buildSliceLabel(slot);
    }

    return visibleSlices;
}

std::vector<SampleSlice> SliceModel::buildUniformSlices(const LoadedSampleData& sampleData,
                                                        int totalSliceCount)
{
    std::vector<SampleSlice> slices;

    const auto totalSamples = sampleData.sourceLengthSamples;
    if (totalSamples <= 0)
        return slices;

    const int sliceCount = juce::jmax(1, totalSliceCount);
    slices.reserve(static_cast<size_t>(sliceCount));

    for (int index = 0; index < sliceCount; ++index)
    {
        SampleSlice slice;
        slice.startSample = (totalSamples * index) / sliceCount;
        slice.endSample = (totalSamples * (index + 1)) / sliceCount;
        if (slice.endSample <= slice.startSample)
            slice.endSample = juce::jmin(totalSamples, slice.startSample + 1);

        fillSliceMetadata(slice, index, totalSamples);
        slices.push_back(std::move(slice));
    }

    return slices;
}

std::vector<SampleSlice> SliceModel::buildBeatSlices(const LoadedSampleData& sampleData,
                                                     double tempoBpm,
                                                     int beatDivision)
{
    if (sampleData.sourceLengthSamples <= 0 || sampleData.sourceSampleRate <= 0.0 || !(tempoBpm > 0.0))
        return {};

    const int safeBeatDivision = juce::jlimit(1, 8, beatDivision);
    const double beatSeconds = 60.0 / tempoBpm;
    const double sliceSeconds = beatSeconds / static_cast<double>(safeBeatDivision);
    const int64_t sliceSamples = static_cast<int64_t>(std::round(sliceSeconds * sampleData.sourceSampleRate));
    if (sliceSamples <= 0)
        return {};

    std::vector<SampleSlice> slices;
    const int totalSliceCount = juce::jlimit(SliceModel::VisibleSliceCount,
                                             kMaxInitialSliceCount,
                                             juce::jmax(SliceModel::VisibleSliceCount,
                                                        static_cast<int>(std::ceil(sampleData.sourceLengthSamples / static_cast<double>(sliceSamples)))));
    slices.reserve(static_cast<size_t>(totalSliceCount));

    for (int index = 0; index < totalSliceCount; ++index)
    {
        SampleSlice slice;
        slice.startSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, static_cast<int64_t>(index) * sliceSamples);
        slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + sliceSamples);
        if (slice.endSample <= slice.startSample)
            slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + 1);
        fillSliceMetadata(slice, index, sampleData.sourceLengthSamples);
        slices.push_back(std::move(slice));
    }

    return slices;
}

std::vector<SampleSlice> SliceModel::buildTransientSlices(const LoadedSampleData& sampleData,
                                                          const std::vector<int64_t>& transientSamples)
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    if (transientSamples.empty())
        return buildUniformSlices(sampleData, SliceModel::VisibleSliceCount);

    std::vector<int64_t> starts;
    starts.reserve(transientSamples.size() + 1);
    starts.push_back(0);
    for (auto samplePosition : transientSamples)
    {
        if (samplePosition <= 0 || samplePosition >= sampleData.sourceLengthSamples)
            continue;

        if (!starts.empty() && (samplePosition - starts.back()) < kMinTransientGapSamples)
            continue;

        starts.push_back(samplePosition);
        if (static_cast<int>(starts.size()) >= kMaxInitialSliceCount)
            break;
    }

    std::vector<SampleSlice> slices;
    slices.reserve(starts.size());
    for (size_t index = 0; index < starts.size(); ++index)
    {
        SampleSlice slice;
        slice.startSample = starts[index];
        slice.endSample = (index + 1 < starts.size())
            ? starts[index + 1]
            : sampleData.sourceLengthSamples;
        if (slice.endSample <= slice.startSample)
            continue;

        slice.transientDerived = true;
        fillSliceMetadata(slice, static_cast<int>(index), sampleData.sourceLengthSamples);
        slices.push_back(std::move(slice));
    }

    return slices.empty()
        ? buildUniformSlices(sampleData, SliceModel::VisibleSliceCount)
        : slices;
}

std::vector<SampleSlice> SliceModel::buildManualSlices(const LoadedSampleData& sampleData,
                                                       const std::vector<SampleCuePoint>& cuePoints)
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    if (cuePoints.empty())
        return buildUniformSlices(sampleData, SliceModel::VisibleSliceCount);

    std::vector<int64_t> starts;
    starts.reserve(cuePoints.size() + 1);
    starts.push_back(0);

    for (const auto& cue : cuePoints)
    {
        if (cue.samplePosition <= 0 || cue.samplePosition >= sampleData.sourceLengthSamples)
            continue;
        starts.push_back(cue.samplePosition);
    }

    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    std::vector<SampleSlice> slices;
    slices.reserve(starts.size());
    for (size_t index = 0; index < starts.size(); ++index)
    {
        SampleSlice slice;
        slice.startSample = starts[index];
        slice.endSample = (index + 1 < starts.size())
            ? starts[index + 1]
            : sampleData.sourceLengthSamples;
        if (slice.endSample <= slice.startSample)
            continue;

        slice.manualDerived = true;
        fillSliceMetadata(slice, static_cast<int>(index), sampleData.sourceLengthSamples);
        slices.push_back(std::move(slice));
    }

    return slices.empty()
        ? buildUniformSlices(sampleData, SliceModel::VisibleSliceCount)
        : slices;
}

void SampleAnalysisEngine::enrichAnalysisForFile(const juce::File& file, LoadedSampleData& sampleData) const
{
    if (const auto audioFluxSummary = runAudioFluxAnalysis(file))
    {
        sampleData.analysis = *audioFluxSummary;
        if (sampleData.analysis.estimatedPitchMidi < 0)
            sampleData.analysis.estimatedPitchMidi = pitchHzToMidi(sampleData.analysis.estimatedPitchHz);
        return;
    }

    sampleData.analysis = buildInternalAnalysis(sampleData);
}

std::vector<SampleSlice> SampleAnalysisEngine::buildSlicesForState(const LoadedSampleData& sampleData,
                                                                   const SampleModePersistentState& state) const
{
    if (!state.storedSlices.empty())
    {
        auto slices = state.storedSlices;
        for (size_t i = 0; i < slices.size(); ++i)
            fillSliceMetadata(slices[i], static_cast<int>(i), sampleData.sourceLengthSamples);
        return slices;
    }

    switch (state.sliceMode)
    {
        case SampleSliceMode::Manual:
            return SliceModel::buildManualSlices(sampleData, state.cuePoints);
        case SampleSliceMode::Transient:
            return SliceModel::buildTransientSlices(sampleData, sampleData.analysis.transientSamples);
        case SampleSliceMode::Beat:
            return SliceModel::buildBeatSlices(sampleData,
                                               state.analyzedTempoBpm > 0.0
                                                   ? state.analyzedTempoBpm
                                                   : sampleData.analysis.estimatedTempoBpm,
                                               state.beatDivision);
        case SampleSliceMode::Uniform:
        default:
            break;
    }

    const auto estimatedSliceCount = static_cast<int>(std::ceil(sampleData.durationSeconds / kInitialSliceTargetSeconds));
    const int totalSliceCount = juce::jlimit(SliceModel::VisibleSliceCount,
                                             kMaxInitialSliceCount,
                                             juce::jmax(SliceModel::VisibleSliceCount, estimatedSliceCount));

    return SliceModel::buildUniformSlices(sampleData, totalSliceCount);
}

class SampleFileManager::LoadJob : public juce::ThreadPoolJob
{
public:
    LoadJob(SampleFileManager& ownerToUse,
            juce::File fileToLoad,
            int requestIdToUse,
            LoadCallback callbackToUse)
        : juce::ThreadPoolJob("SampleModeLoadJob")
        , owner(ownerToUse)
        , file(std::move(fileToLoad))
        , requestId(requestIdToUse)
        , callback(std::move(callbackToUse))
    {
    }

    JobStatus runJob() override
    {
        auto decodeResult = owner.decodeFile(file, requestId);

        if (shouldExit())
            return jobHasFinished;

        auto callbackCopy = callback;
        juce::MessageManager::callAsync([mainThreadCallback = std::move(callbackCopy),
                                         asyncResult = std::move(decodeResult)]() mutable
        {
            if (mainThreadCallback)
                mainThreadCallback(std::move(asyncResult));
        });

        return jobHasFinished;
    }

private:
    SampleFileManager& owner;
    juce::File file;
    int requestId = 0;
    LoadCallback callback;
};

SampleFileManager::SampleFileManager()
{
    formatManager.registerBasicFormats();
}

SampleFileManager::~SampleFileManager()
{
    cancelPendingLoads();
}

int SampleFileManager::loadFileAsync(const juce::File& file, LoadCallback callback)
{
    const int requestId = nextRequestId.fetch_add(1);
    threadPool.addJob(new LoadJob(*this, file, requestId, std::move(callback)), true);
    return requestId;
}

void SampleFileManager::cancelPendingLoads()
{
    threadPool.removeAllJobs(true, 10000);
}

SampleFileManager::LoadResult SampleFileManager::decodeFile(const juce::File& file, int requestId)
{
    LoadResult result;
    result.sourceFile = file;
    result.requestId = requestId;

    if (!file.existsAsFile())
    {
        result.errorMessage = "File does not exist";
        return result;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        result.errorMessage = "Unsupported or unreadable audio file";
        return result;
    }

    if (reader->lengthInSamples <= 0)
    {
        result.errorMessage = "Audio file is empty";
        return result;
    }

    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        result.errorMessage = "Audio file is too large for the current Flip mode MVP";
        return result;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int sourceChannelCount = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int decodeChannelCount = juce::jmin(2, sourceChannelCount);

    juce::AudioBuffer<float> sourceBuffer(decodeChannelCount, numSamples);
    sourceBuffer.clear();

    if (!reader->read(&sourceBuffer, 0, numSamples, 0, true, decodeChannelCount > 1))
    {
        result.errorMessage = "Failed to decode audio file";
        return result;
    }

    auto loadedSample = std::make_shared<LoadedSampleData>();
    loadedSample->audioBuffer.setSize(2, numSamples, false, false, true);
    loadedSample->audioBuffer.clear();

    if (decodeChannelCount == 1)
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }
    else
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 1, 0, numSamples);
    }

    loadedSample->sourcePath = file.getFullPathName();
    loadedSample->displayName = file.getFileNameWithoutExtension();
    loadedSample->sourceSampleRate = reader->sampleRate;
    loadedSample->numChannels = decodeChannelCount;
    loadedSample->sourceLengthSamples = reader->lengthInSamples;
    loadedSample->durationSeconds = reader->lengthInSamples / juce::jmax(1.0, reader->sampleRate);
    buildWaveformPreview(*loadedSample);
    SampleAnalysisEngine().enrichAnalysisForFile(file, *loadedSample);

    result.success = true;
    result.loadedSample = std::const_pointer_cast<const LoadedSampleData>(loadedSample);
    return result;
}

void SamplePlaybackVoice::reset()
{
    active = false;
    releasing = false;
    loopEnabled = false;
    visibleSlot = -1;
    voiceId = 0;
    activeSlice = {};
    readPosition = 0.0;
    stretchedReadPosition = 0.0;
    fadeInRemaining = 0;
    fadeInTotal = 0;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
    stretchedBuffer.reset();
}

void SamplePlaybackVoice::trigger(const SampleSlice& slice,
                                  int visibleSlotToUse,
                                  uint64_t voiceIdToUse,
                                  bool loopEnabledToUse,
                                  int fadeSamples)
{
    active = true;
    releasing = false;
    loopEnabled = loopEnabledToUse;
    visibleSlot = visibleSlotToUse;
    voiceId = voiceIdToUse;
    activeSlice = slice;
    readPosition = static_cast<double>(slice.startSample);
    stretchedReadPosition = 0.0;
    fadeInTotal = juce::jmax(0, fadeSamples);
    fadeInRemaining = fadeInTotal;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
    stretchedBuffer.reset();
}

void SamplePlaybackVoice::beginRelease(int fadeSamples)
{
    if (!active)
        return;

    releasing = true;
    fadeOutTotal = juce::jmax(0, fadeSamples);
    fadeOutRemaining = fadeOutTotal;
}

void SamplePlaybackVoice::stop()
{
    reset();
}

void SamplePlaybackVoice::setStretchedBuffer(std::shared_ptr<const StretchedSliceBuffer> bufferToUse)
{
    stretchedBuffer = std::move(bufferToUse);
    if (stretchedBuffer == nullptr)
    {
        stretchedReadPosition = 0.0;
        return;
    }

    const double sliceLength = juce::jmax(1.0, static_cast<double>(activeSlice.endSample - activeSlice.startSample));
    const double normalized = juce::jlimit(0.0,
                                           1.0,
                                           (readPosition - static_cast<double>(activeSlice.startSample)) / sliceLength);
    const double cacheLength = juce::jmax(1, stretchedBuffer->audioBuffer.getNumSamples());
    stretchedReadPosition = normalized * static_cast<double>(cacheLength);
}

void SamplePlaybackVoice::clearStretchedBuffer()
{
    stretchedBuffer.reset();
    stretchedReadPosition = 0.0;
}

SampleModeEngine::SampleModeEngine() = default;

SampleModeEngine::~SampleModeEngine()
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);
}

void SampleModeEngine::prepare(double sampleRate, int maxBlockSize)
{
    preparedSampleRate.store(juce::jmax(1.0, sampleRate), std::memory_order_release);
    preparedMaxBlockSize.store(juce::jmax(1, maxBlockSize), std::memory_order_release);
}

bool SampleModeEngine::loadSampleFromBuffer(const juce::AudioBuffer<float>& buffer,
                                            double sourceSampleRate,
                                            const juce::String& sourcePath,
                                            const juce::String& displayName)
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);
    auto sample = buildLoadedSampleFromBuffer(buffer, sourceSampleRate, sourcePath, displayName);
    if (sample == nullptr)
        return false;

    {
        const juce::ScopedLock lock(stateLock);
        loadedSample = std::move(sample);
        persistentState.samplePath = sourcePath;
        rebuildSlicesLocked();
        sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
        isLoading = false;
        activeRequestId = 0;
        pendingTriggerSliceValid = false;
        statusText = "Loaded " + loadedSample->displayName;
        keyLockCaches.clear();
        keyLockCacheBuildInFlight = false;
        ++keyLockCacheGeneration;
    }

    stop(true);
    clearPendingVisibleSlice();
    sendChangeMessage();
    return true;
}

int SampleModeEngine::loadSampleAsync(const juce::File& file)
{
    if (!file.existsAsFile())
        return 0;

    juce::WeakReference<SampleModeEngine> weakThis(this);

    {
        const juce::ScopedLock lock(stateLock);
        isLoading = true;
        statusText = "Loading " + file.getFileName() + "...";
        persistentState.samplePath = file.getFullPathName();
        activeRequestId = fileManager.loadFileAsync(file,
            [weakThis](SampleFileManager::LoadResult result) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadResult(std::move(result));
            });
    }

    sendChangeMessage();
    return activeRequestId;
}

void SampleModeEngine::clear()
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);

    {
        const juce::ScopedLock lock(stateLock);
        loadedSample.reset();
        sliceModel.clear();
        persistentState = {};
        isLoading = false;
        activeRequestId = 0;
        statusText = "No sample loaded";
        keyLockCaches.clear();
        keyLockCacheEnabled = false;
        keyLockCacheBuildInFlight = false;
        ++keyLockCacheGeneration;
    }

    stop();
    clearPendingVisibleSlice();

    sendChangeMessage();
}

void SampleModeEngine::stop(bool immediate)
{
    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (!immediate)
    {
        const int fadeSamples = juce::jmax(16,
            juce::jmin(kDefaultSliceFadeSamples,
                       static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));
        bool anyActive = false;
        for (auto& voice : playbackVoices)
        {
            if (voice.isActive())
            {
                voice.beginRelease(fadeSamples);
                anyActive = true;
            }
        }
        if (anyActive)
        {
            playingAtomic.store(1, std::memory_order_release);
            return;
        }
    }

    for (auto& voice : playbackVoices)
        voice.stop();
    activeVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(-1.0f, std::memory_order_release);
    playingAtomic.store(0, std::memory_order_release);
}

bool SampleModeEngine::hasSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample != nullptr;
}

SampleModeEngine::StateSnapshot SampleModeEngine::getStateSnapshot() const
{
    const juce::ScopedLock lock(stateLock);

    StateSnapshot snapshot;
    snapshot.hasSample = (loadedSample != nullptr);
    snapshot.isLoading = isLoading;
    snapshot.statusText = statusText;
    snapshot.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
    snapshot.visibleSliceBankCount = sliceModel.getVisibleBankCount();
    snapshot.canNavigateLeft = sliceModel.canNavigateLeft();
    snapshot.canNavigateRight = sliceModel.canNavigateRight();
    snapshot.isPlaying = (playingAtomic.load(std::memory_order_acquire) != 0);
    snapshot.activeVisibleSliceSlot = activeVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.pendingVisibleSliceSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.playbackProgress = playbackProgress.load(std::memory_order_acquire);
    snapshot.visibleSlices = sliceModel.getVisibleSlices();
    snapshot.sliceMode = persistentState.sliceMode;
    snapshot.triggerMode = persistentState.triggerMode;
    snapshot.beatDivision = persistentState.beatDivision;
    snapshot.viewZoom = persistentState.viewZoom;
    snapshot.viewScroll = persistentState.viewScroll;
    snapshot.selectedCueIndex = persistentState.selectedCueIndex;
    snapshot.estimatedTempoBpm = persistentState.analyzedTempoBpm;
    snapshot.estimatedPitchHz = persistentState.analyzedPitchHz;
    snapshot.estimatedPitchMidi = persistentState.analyzedPitchMidi;
    snapshot.analysisSource = persistentState.analysisSource;
    snapshot.cuePoints = persistentState.cuePoints;

    if (loadedSample != nullptr)
    {
        snapshot.samplePath = loadedSample->sourcePath;
        snapshot.displayName = loadedSample->displayName;
        snapshot.sourceSampleRate = loadedSample->sourceSampleRate;
        snapshot.totalSamples = loadedSample->sourceLengthSamples;
    }

    return snapshot;
}

std::shared_ptr<const LoadedSampleData> SampleModeEngine::getLoadedSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample;
}

bool SampleModeEngine::canNavigateVisibleBankLeft() const
{
    const juce::ScopedLock lock(stateLock);
    return sliceModel.canNavigateLeft();
}

bool SampleModeEngine::canNavigateVisibleBankRight() const
{
    const juce::ScopedLock lock(stateLock);
    return sliceModel.canNavigateRight();
}

void SampleModeEngine::stepVisibleBank(int delta)
{
    {
        const juce::ScopedLock lock(stateLock);
        sliceModel.stepVisibleBank(delta);
        persistentState.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
        statusText = loadedSample != nullptr
            ? ("Viewing slice bank " + juce::String(sliceModel.getVisibleBankIndex() + 1)
               + "/" + juce::String(juce::jmax(1, sliceModel.getVisibleBankCount())))
            : juce::String("No sample loaded");
    }

    sendChangeMessage();
}

bool SampleModeEngine::hasVisibleSlice(int visibleSlot) const
{
    SampleSlice slice;
    return resolveVisibleSlice(visibleSlot, slice);
}

int SampleModeEngine::getActiveVisibleSliceSlot() const noexcept
{
    return activeVisibleSliceSlot.load(std::memory_order_acquire);
}

int SampleModeEngine::getPendingVisibleSliceSlot() const noexcept
{
    return pendingVisibleSliceSlot.load(std::memory_order_acquire);
}

void SampleModeEngine::setPendingVisibleSlice(int visibleSlot)
{
    const int clampedSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, visibleSlot);
    SampleSlice resolvedSlice;
    const bool hasSlice = resolveVisibleSlice(clampedSlot, resolvedSlice);
    {
        const juce::ScopedLock lock(stateLock);
        pendingTriggerSliceValid = hasSlice;
        if (hasSlice)
            pendingTriggerSlice = resolvedSlice;
    }
    pendingVisibleSliceSlot.store(clampedSlot, std::memory_order_release);
}

void SampleModeEngine::clearPendingVisibleSlice()
{
    const juce::ScopedLock lock(stateLock);
    pendingTriggerSliceValid = false;
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
}

void SampleModeEngine::requestKeyLockRenderCache(float playbackRate,
                                                 float pitchSemitones,
                                                 bool enabled)
{
#if !MLRVST_ENABLE_SOUNDTOUCH
    juce::ignoreUnused(playbackRate, pitchSemitones, enabled);
    return;
#else
    juce::WeakReference<SampleModeEngine> weakThis(this);
    std::shared_ptr<const LoadedSampleData> sampleData;
    std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    uint64_t generation = 0;
    int visibleBankIndex = 0;
    float quantizedRate = quantizeKeyLockRate(playbackRate);
    float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);

    {
        const juce::ScopedLock lock(stateLock);
        keyLockCacheEnabled = enabled;

        if (!enabled || loadedSample == nullptr)
        {
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
            return;
        }

        if (std::abs(quantizedRate - 1.0f) <= kKeyLockCacheRateTolerance
            && std::abs(quantizedPitch) <= kKeyLockCachePitchTolerance)
        {
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
            return;
        }

        const int currentBankIndex = sliceModel.getVisibleBankIndex();
        const bool matchesExisting = !keyLockCaches.empty()
            && currentBankIndex == keyLockCacheVisibleBankIndex
            && std::abs(keyLockCachePlaybackRate - quantizedRate) <= kKeyLockCacheRateTolerance
            && std::abs(keyLockCachePitchSemitones - quantizedPitch) <= kKeyLockCachePitchTolerance;
        if (matchesExisting || keyLockCacheBuildInFlight)
            return;

        sampleData = loadedSample;
        visibleSlices = sliceModel.getVisibleSlices();
        visibleBankIndex = currentBankIndex;
        keyLockCacheBuildInFlight = true;
        generation = ++keyLockCacheGeneration;
    }

    class KeyLockCacheJob : public juce::ThreadPoolJob
    {
    public:
        KeyLockCacheJob(juce::WeakReference<SampleModeEngine> ownerToUse,
                        std::shared_ptr<const LoadedSampleData> sampleDataToUse,
                        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlicesToUse,
                        uint64_t generationToUse,
                        float playbackRateToUse,
                        float pitchSemitonesToUse,
                        int visibleBankIndexToUse)
            : juce::ThreadPoolJob("FlipKeyLockCache")
            , owner(ownerToUse)
            , sampleData(std::move(sampleDataToUse))
            , visibleSlices(std::move(visibleSlicesToUse))
            , generation(generationToUse)
            , playbackRate(playbackRateToUse)
            , pitchSemitones(pitchSemitonesToUse)
            , visibleBankIndex(visibleBankIndexToUse)
        {
        }

        JobStatus runJob() override
        {
            std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches;
            if (sampleData != nullptr)
            {
                caches.reserve(SliceModel::VisibleSliceCount);
                for (const auto& slice : visibleSlices)
                {
                    if (shouldExit())
                        return jobHasFinished;
                    if (slice.id < 0 || slice.endSample <= slice.startSample)
                        continue;

                    if (auto cache = buildStretchedSliceBuffer(*sampleData, slice, playbackRate, pitchSemitones))
                        caches.push_back(std::move(cache));
                }
            }

            juce::MessageManager::callAsync(
                [weakOwner = owner,
                 generationValue = generation,
                 readyCaches = std::move(caches),
                 rate = playbackRate,
                 pitch = pitchSemitones,
                 bankIndex = visibleBankIndex]() mutable
                {
                    if (weakOwner != nullptr)
                    {
                        weakOwner->handleKeyLockCacheReady(generationValue,
                                                           std::move(readyCaches),
                                                           rate,
                                                           pitch,
                                                           bankIndex);
                    }
                });
            return jobHasFinished;
        }

    private:
        juce::WeakReference<SampleModeEngine> owner;
        std::shared_ptr<const LoadedSampleData> sampleData;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
        uint64_t generation = 0;
        float playbackRate = 1.0f;
        float pitchSemitones = 0.0f;
        int visibleBankIndex = 0;
    };

    keyLockCacheThreadPool.addJob(
        new KeyLockCacheJob(weakThis,
                            std::move(sampleData),
                            visibleSlices,
                            generation,
                            quantizedRate,
                            quantizedPitch,
                            visibleBankIndex),
        true);
#endif
}

bool SampleModeEngine::triggerVisibleSlice(int visibleSlot, bool loopEnabled)
{
    SampleSlice slice;
    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    const int pendingSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    {
        const juce::ScopedLock lock(stateLock);
        triggerMode = loopEnabled ? SampleTriggerMode::Loop : persistentState.triggerMode;
        if (pendingTriggerSliceValid && pendingSlot == visibleSlot)
        {
            slice = pendingTriggerSlice;
            pendingTriggerSliceValid = false;
        }
        else
        {
            if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
                return false;

            const auto visibleSlices = sliceModel.getVisibleSlices();
            const auto& resolvedSlice = visibleSlices[static_cast<size_t>(visibleSlot)];
            if (resolvedSlice.id < 0 || resolvedSlice.endSample <= resolvedSlice.startSample)
                return false;

            slice = resolvedSlice;
        }
    }

    const int fadeSamples = juce::jmax(16,
        juce::jmin(kDefaultSliceFadeSamples,
                   static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (triggerMode == SampleTriggerMode::Loop)
    {
        for (auto& voice : playbackVoices)
        {
            if (voice.isActive())
                voice.beginRelease(fadeSamples);
        }
    }

    SamplePlaybackVoice* targetVoice = nullptr;
    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
        {
            targetVoice = &voice;
            break;
        }
    }
    if (targetVoice == nullptr)
    {
        targetVoice = &playbackVoices.front();
        for (auto& voice : playbackVoices)
        {
            if (voice.getVoiceId() < targetVoice->getVoiceId())
                targetVoice = &voice;
        }
        targetVoice->stop();
    }

    targetVoice->trigger(slice, visibleSlot, nextVoiceId++, triggerMode == SampleTriggerMode::Loop, fadeSamples);
    activeVisibleSliceSlot.store(visibleSlot, std::memory_order_release);
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(slice.normalizedStart, std::memory_order_release);
    playingAtomic.store(1, std::memory_order_release);
    return true;
}

SampleModeEngine::RenderResult SampleModeEngine::renderToBuffer(juce::AudioBuffer<float>& output,
                                                                int startSample,
                                                                int numSamples,
                                                                float playbackRate,
                                                                int fadeSamples,
                                                                float pitchSemitones,
                                                                bool preferHighQualityKeyLock)
{
    RenderResult result;
    if (numSamples <= 0)
        return result;

    std::shared_ptr<const LoadedSampleData> sampleData;
    {
        const juce::ScopedLock lock(stateLock);
        sampleData = loadedSample;
    }

    if (sampleData == nullptr || sampleData->audioBuffer.getNumSamples() <= 0)
    {
        stop(true);
        return result;
    }

    const double renderRate = preparedSampleRate.load(std::memory_order_acquire);
    const double sourceRate = juce::jmax(1.0, sampleData->sourceSampleRate);
    const double rateScale = sourceRate / juce::jmax(1.0, renderRate);
    const double sourceIncrement = juce::jlimit(0.03125, 8.0, static_cast<double>(playbackRate)) * rateScale;
    const double cacheIncrement = rateScale;
    const int fadeLen = juce::jmax(16, fadeSamples);
    std::vector<std::shared_ptr<const StretchedSliceBuffer>> availableKeyLockCaches;

    bool renderedAnything = false;
    int newestActiveSlot = -1;
    uint64_t newestVoiceId = 0;
    float latestProgress = playbackProgress.load(std::memory_order_acquire);
    bool canUseKeyLockForAllVoices = false;

    if (preferHighQualityKeyLock)
    {
        const juce::ScopedLock stateScopedLock(stateLock);
        availableKeyLockCaches = keyLockCaches;
    }

    auto findAvailableCache = [&](const SampleSlice& slice) -> std::shared_ptr<const StretchedSliceBuffer>
    {
        for (const auto& cache : availableKeyLockCaches)
        {
            if (cache != nullptr && matchesKeyLockRequest(*cache, slice, playbackRate, pitchSemitones))
                return cache;
        }
        return {};
    };

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (preferHighQualityKeyLock)
    {
        canUseKeyLockForAllVoices = true;
        for (auto& voice : playbackVoices)
        {
            if (!voice.isActive())
                continue;

            const auto existing = voice.getStretchedBuffer();
            const bool hasMatchingExisting = existing != nullptr
                && matchesKeyLockRequest(*existing, voice.getActiveSlice(), playbackRate, pitchSemitones);
            if (!hasMatchingExisting && findAvailableCache(voice.getActiveSlice()) == nullptr)
            {
                canUseKeyLockForAllVoices = false;
                break;
            }
        }

        if (canUseKeyLockForAllVoices)
        {
            for (auto& voice : playbackVoices)
            {
                if (!voice.isActive())
                    continue;

                const auto existing = voice.getStretchedBuffer();
                const bool hasMatchingExisting = existing != nullptr
                    && matchesKeyLockRequest(*existing, voice.getActiveSlice(), playbackRate, pitchSemitones);
                if (!hasMatchingExisting)
                {
                    if (auto cache = findAvailableCache(voice.getActiveSlice()))
                        voice.setStretchedBuffer(std::move(cache));
                    else
                        canUseKeyLockForAllVoices = false;
                }
            }
        }
    }

    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
            continue;

        renderedAnything = true;
        if (voice.getVoiceId() >= newestVoiceId)
        {
            newestVoiceId = voice.getVoiceId();
            newestActiveSlot = voice.getVisibleSlot();
        }

        const double sliceStart = static_cast<double>(voice.getActiveSlice().startSample);
        const double sliceEnd = static_cast<double>(juce::jmax(voice.getActiveSlice().startSample + 1,
                                                               voice.getActiveSlice().endSample));
        const double sliceLength = juce::jmax(1.0, sliceEnd - sliceStart);
        const double localFade = juce::jmin(sliceLength * 0.25, static_cast<double>(fadeLen));
        const auto stretchedBuffer = canUseKeyLockForAllVoices
            ? voice.getStretchedBuffer()
            : std::shared_ptr<const StretchedSliceBuffer>();
        const bool usingStretched = (stretchedBuffer != nullptr);
        const double stretchedLength = usingStretched
            ? static_cast<double>(juce::jmax(1, stretchedBuffer->audioBuffer.getNumSamples()))
            : 0.0;
        const double stretchedFade = usingStretched
            ? juce::jmin(stretchedLength * 0.25, static_cast<double>(fadeLen))
            : 0.0;

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            if (!voice.isActive())
                break;

            if (!voice.shouldLoop() && voice.readPosition >= sliceEnd)
            {
                voice.stop();
                break;
            }

            if (voice.shouldLoop())
            {
                while (voice.readPosition >= sliceEnd)
                    voice.readPosition -= sliceLength;
                while (voice.readPosition < sliceStart)
                    voice.readPosition += sliceLength;
            }

            if (usingStretched)
            {
                if (voice.shouldLoop())
                {
                    while (voice.stretchedReadPosition >= stretchedLength)
                        voice.stretchedReadPosition -= stretchedLength;
                    while (voice.stretchedReadPosition < 0.0)
                        voice.stretchedReadPosition += stretchedLength;
                }
                else
                {
                    voice.stretchedReadPosition = juce::jlimit(0.0, stretchedLength, voice.stretchedReadPosition);
                }
            }

            float gain = 1.0f;
            const double fromStart = juce::jmax(0.0, voice.readPosition - sliceStart);
            const double toEnd = juce::jmax(0.0, sliceEnd - voice.readPosition);

            if (localFade > 1.0)
            {
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, fromStart / localFade));
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, toEnd / localFade));
            }

            if (usingStretched && stretchedFade > 1.0)
            {
                const double stretchedFromStart = juce::jmax(0.0, voice.stretchedReadPosition);
                const double stretchedToEnd = juce::jmax(0.0, stretchedLength - voice.stretchedReadPosition);
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, stretchedFromStart / stretchedFade));
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, stretchedToEnd / stretchedFade));
            }

            if (voice.fadeInRemaining > 0 && voice.fadeInTotal > 0)
            {
                const float fadeInGain = 1.0f - (static_cast<float>(voice.fadeInRemaining) / static_cast<float>(voice.fadeInTotal));
                gain *= juce::jlimit(0.0f, 1.0f, fadeInGain);
                --voice.fadeInRemaining;
            }

            if (voice.isReleasing())
            {
                if (voice.fadeOutRemaining > 0 && voice.fadeOutTotal > 0)
                {
                    const float fadeOutGain = static_cast<float>(voice.fadeOutRemaining) / static_cast<float>(voice.fadeOutTotal);
                    gain *= juce::jlimit(0.0f, 1.0f, fadeOutGain);
                    --voice.fadeOutRemaining;
                }
                else
                {
                    voice.stop();
                    break;
                }
            }

            if (gain > 1.0e-5f)
            {
                const float left = usingStretched
                    ? readInterpolatedSample(stretchedBuffer->audioBuffer, 0, voice.stretchedReadPosition)
                    : readInterpolatedSample(sampleData->audioBuffer, 0, voice.readPosition);
                const float right = usingStretched
                    ? readInterpolatedSample(stretchedBuffer->audioBuffer, 1, voice.stretchedReadPosition)
                    : readInterpolatedSample(sampleData->audioBuffer, 1, voice.readPosition);
                output.addSample(0, startSample + sampleIndex, left * gain);
                output.addSample(1, startSample + sampleIndex, right * gain);
            }

            voice.readPosition += sourceIncrement;
            if (usingStretched)
                voice.stretchedReadPosition += cacheIncrement;
            latestProgress = juce::jlimit(0.0f,
                                          1.0f,
                                          static_cast<float>(voice.readPosition
                                              / juce::jmax<int64_t>(1, sampleData->sourceLengthSamples)));
        }
    }

    activeVisibleSliceSlot.store(newestActiveSlot, std::memory_order_release);
    playbackProgress.store(renderedAnything ? latestProgress : -1.0f, std::memory_order_release);
    playingAtomic.store(renderedAnything ? 1 : 0, std::memory_order_release);
    result.renderedAnything = renderedAnything;
    result.usedInternalPitch = renderedAnything && canUseKeyLockForAllVoices;
    return result;
}

void SampleModeEngine::setLoadStatusCallback(LoadStatusCallback callback)
{
    const juce::ScopedLock lock(stateLock);
    loadStatusCallback = std::move(callback);
}

SampleModePersistentState SampleModeEngine::capturePersistentState() const
{
    const juce::ScopedLock lock(stateLock);
    auto state = persistentState;
    state.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
    state.storedSlices = sliceModel.getAllSlices();
    if (loadedSample != nullptr)
    {
        state.samplePath = loadedSample->sourcePath;
        state.analyzedTempoBpm = loadedSample->analysis.estimatedTempoBpm;
        state.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
        state.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
        state.analysisSource = loadedSample->analysis.analysisSource;
    }
    return state;
}

void SampleModeEngine::applyPersistentState(const SampleModePersistentState& state)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState = state;
        persistentState.beatDivision = juce::jlimit(1, 8, persistentState.beatDivision);
        persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, persistentState.viewZoom);
        persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, persistentState.viewScroll);
        persistentState.selectedCueIndex = clampCueSelection(persistentState.selectedCueIndex,
                                                             static_cast<int>(persistentState.cuePoints.size()));
        if (loadedSample != nullptr)
        {
            rebuildSlicesLocked();
            sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
        }
    }

    sendChangeMessage();
}

void SampleModeEngine::setSliceMode(SampleSliceMode mode)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.sliceMode = mode;
        persistentState.storedSlices.clear();
        if (mode != SampleSliceMode::Manual)
            persistentState.selectedCueIndex = -1;
        rebuildSlicesLocked();
    }

    sendChangeMessage();
}

SampleSliceMode SampleModeEngine::getSliceMode() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.sliceMode;
}

void SampleModeEngine::setTriggerMode(SampleTriggerMode mode)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.triggerMode = mode;
    }
    sendChangeMessage();
}

SampleTriggerMode SampleModeEngine::getTriggerMode() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.triggerMode;
}

void SampleModeEngine::setBeatDivision(int division)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.beatDivision = juce::jlimit(1, 8, division);
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Beat)
            rebuildSlicesLocked();
    }
    sendChangeMessage();
}

int SampleModeEngine::getBeatDivision() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.beatDivision;
}

void SampleModeEngine::setViewWindow(float scroll, float zoom)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, zoom);
        persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, scroll);
    }
    sendChangeMessage();
}

float SampleModeEngine::getViewScroll() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.viewScroll;
}

float SampleModeEngine::getViewZoom() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.viewZoom;
}

void SampleModeEngine::selectCuePoint(int cueIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.selectedCueIndex = clampCueSelection(cueIndex, static_cast<int>(persistentState.cuePoints.size()));
    }
    sendChangeMessage();
}

int SampleModeEngine::getSelectedCuePoint() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.selectedCueIndex;
}

int SampleModeEngine::createCuePointAtNormalizedPosition(float normalizedPosition)
{
    int selectedIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        const int64_t cueSample = static_cast<int64_t>(
            std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition) * static_cast<float>(loadedSample->sourceLengthSamples - 1)));
        if (cueSample <= 0 || cueSample >= loadedSample->sourceLengthSamples)
            return -1;

        SampleCuePoint cue;
        cue.id = static_cast<int>(persistentState.cuePoints.size());
        cue.samplePosition = cueSample;
        cue.name = buildCueName(cue.id);
        cue.loopEndSample = loadedSample->sourceLengthSamples;
        persistentState.cuePoints.push_back(std::move(cue));
        std::sort(persistentState.cuePoints.begin(), persistentState.cuePoints.end(),
                  [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].samplePosition == cueSample)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                break;
            }
        }
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
        selectedIndex = persistentState.selectedCueIndex;
    }

    sendChangeMessage();
    return selectedIndex;
}

bool SampleModeEngine::moveCuePoint(int cueIndex, float normalizedPosition)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || cueIndex < 0
            || cueIndex >= static_cast<int>(persistentState.cuePoints.size()))
        {
            return false;
        }

        const int cueId = persistentState.cuePoints[static_cast<size_t>(cueIndex)].id;
        const int64_t cueSample = static_cast<int64_t>(
            std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition) * static_cast<float>(loadedSample->sourceLengthSamples - 1)));
        persistentState.cuePoints[static_cast<size_t>(cueIndex)].samplePosition = cueSample;
        std::sort(persistentState.cuePoints.begin(), persistentState.cuePoints.end(),
                  [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].id == cueId)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                break;
            }
        }
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return true;
}

bool SampleModeEngine::deleteCuePoint(int cueIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (cueIndex < 0 || cueIndex >= static_cast<int>(persistentState.cuePoints.size()))
            return false;

        persistentState.cuePoints.erase(persistentState.cuePoints.begin() + cueIndex);
        persistentState.selectedCueIndex = clampCueSelection(cueIndex - 1, static_cast<int>(persistentState.cuePoints.size()));
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return true;
}

bool SampleModeEngine::resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return false;

    const juce::ScopedLock lock(stateLock);
    const auto visibleSlices = sliceModel.getVisibleSlices();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return false;

    sliceOut = slice;
    return true;
}

void SampleModeEngine::handleLoadResult(SampleFileManager::LoadResult result)
{
    LoadStatusCallback callbackToInvoke;

    {
        const juce::ScopedLock lock(stateLock);

        if (result.requestId != activeRequestId)
            return;

        isLoading = false;

        if (result.success && result.loadedSample != nullptr)
        {
            loadedSample = std::move(result.loadedSample);
            persistentState.samplePath = loadedSample->sourcePath;
            persistentState.analyzedTempoBpm = loadedSample->analysis.estimatedTempoBpm;
            persistentState.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
            persistentState.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
            persistentState.analysisSource = loadedSample->analysis.analysisSource;
            rebuildSlicesLocked();
            sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
            pendingTriggerSliceValid = false;
            statusText = "Loaded " + loadedSample->displayName;
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
        }
        else
        {
            loadedSample.reset();
            sliceModel.clear();
            pendingTriggerSliceValid = false;
            statusText = result.errorMessage.isNotEmpty() ? result.errorMessage : "Sample load failed";
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
        }

        callbackToInvoke = loadStatusCallback;
    }

    stop(true);
    clearPendingVisibleSlice();

    if (callbackToInvoke)
        callbackToInvoke(result);

    sendChangeMessage();
}

void SampleModeEngine::handleKeyLockCacheReady(uint64_t generation,
                                               std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches,
                                               float playbackRate,
                                               float pitchSemitones,
                                               int visibleBankIndex)
{
    const juce::ScopedLock lock(stateLock);
    if (generation != keyLockCacheGeneration)
        return;

    keyLockCaches = std::move(caches);
    keyLockCachePlaybackRate = playbackRate;
    keyLockCachePitchSemitones = pitchSemitones;
    keyLockCacheVisibleBankIndex = visibleBankIndex;
    keyLockCacheBuildInFlight = false;
}

std::shared_ptr<const StretchedSliceBuffer> SampleModeEngine::findKeyLockCacheForSlice(const SampleSlice& slice,
                                                                                        float playbackRate,
                                                                                        float pitchSemitones) const
{
    const float quantizedRate = quantizeKeyLockRate(playbackRate);
    const float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);
    for (const auto& cache : keyLockCaches)
    {
        if (cache != nullptr && matchesKeyLockRequest(*cache, slice, quantizedRate, quantizedPitch))
            return cache;
    }

    return {};
}

bool SampleModeEngine::voiceHasMatchingKeyLockCache(const SamplePlaybackVoice& voice,
                                                    float playbackRate,
                                                    float pitchSemitones) const
{
    if (const auto existing = voice.getStretchedBuffer())
        return matchesKeyLockRequest(*existing, voice.getActiveSlice(), playbackRate, pitchSemitones);

    return findKeyLockCacheForSlice(voice.getActiveSlice(), playbackRate, pitchSemitones) != nullptr;
}

void SampleModeEngine::rebuildSlicesLocked()
{
    if (loadedSample == nullptr)
    {
        sliceModel.clear();
        return;
    }

    sliceModel.setSlices(analysisEngine.buildSlicesForState(*loadedSample, persistentState));
    sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
    persistentState.storedSlices = sliceModel.getAllSlices();
    keyLockCaches.clear();
    keyLockCacheBuildInFlight = false;
    ++keyLockCacheGeneration;
}

int SampleModeEngine::clampCueSelection(int selectedCueIndex, int cueCount)
{
    if (cueCount <= 0)
        return -1;
    return juce::jlimit(0, cueCount - 1, selectedCueIndex);
}

SampleModeComponent::SampleModeComponent()
{
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

SampleModeComponent::~SampleModeComponent()
{
    stopTimer();
    setEngine(nullptr);
}

void SampleModeComponent::setEngine(SampleModeEngine* engineToUse)
{
    if (engine == engineToUse)
        return;

    if (engine != nullptr)
        engine->removeChangeListener(this);

    engine = engineToUse;

    if (engine != nullptr)
        engine->addChangeListener(this);

    refreshFromEngine();
}

void SampleModeComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(8);
    g.fillAll(juce::Colour(0xff0d1014));

    auto header = bounds.removeFromTop(18);
    g.setColour(juce::Colour(0xffd7dce2));
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    const juce::String headerText = stateSnapshot.displayName.isNotEmpty()
        ? stateSnapshot.displayName
        : stateSnapshot.statusText;
    g.drawText(headerText, header.removeFromLeft(juce::jmax(120, bounds.getWidth() / 2)), juce::Justification::centredLeft);
    if (stateSnapshot.estimatedTempoBpm > 0.0 || stateSnapshot.estimatedPitchMidi >= 0)
    {
        juce::String analysisText;
        if (stateSnapshot.estimatedTempoBpm > 0.0)
            analysisText << juce::String(stateSnapshot.estimatedTempoBpm, 1) << " BPM";
        if (stateSnapshot.estimatedPitchMidi >= 0)
        {
            if (analysisText.isNotEmpty())
                analysisText << "  |  ";
            analysisText << noteNameForMidi(stateSnapshot.estimatedPitchMidi);
        }
        if (stateSnapshot.analysisSource.isNotEmpty())
            analysisText << "  " << stateSnapshot.analysisSource;

        g.setColour(juce::Colour(0xff8d9aab));
        g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
        g.drawText(analysisText, header, juce::Justification::centredRight);
    }
    bounds.removeFromTop(4);

    waveformBounds = bounds.removeFromTop(juce::jmax(170, bounds.getHeight() - 52));
    g.setColour(juce::Colour(0xff171d25));
    g.fillRoundedRectangle(waveformBounds.toFloat(), 8.0f);
    g.setColour(juce::Colour(0xff263241));
    g.drawRoundedRectangle(waveformBounds.toFloat(), 8.0f, 1.0f);

    if (loadedSample != nullptr && !loadedSample->previewMin.empty() && !loadedSample->previewMax.empty())
    {
        const auto visibleRange = getVisibleRange();
        const auto centerY = waveformBounds.getCentreY();
        const auto halfHeight = waveformBounds.getHeight() * 0.42f;
        const auto pointCount = static_cast<int>(loadedSample->previewMin.size());
        const float visibleStart = visibleRange.getStart();
        const float visibleEnd = visibleRange.getEnd();
        const float visibleWidth = juce::jmax(1.0e-5f, visibleEnd - visibleStart);

        g.setColour(juce::Colour(0xff6ca7ff));
        for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
        {
            const float pointNorm = static_cast<float>(pointIndex) / juce::jmax(1, pointCount - 1);
            if (pointNorm < visibleStart || pointNorm > visibleEnd)
                continue;
            const float visibleNorm = (pointNorm - visibleStart) / visibleWidth;
            const float x = waveformBounds.getX() + (waveformBounds.getWidth() - 1.0f) * visibleNorm;
            const float minY = centerY - (loadedSample->previewMin[static_cast<size_t>(pointIndex)] * halfHeight);
            const float maxY = centerY - (loadedSample->previewMax[static_cast<size_t>(pointIndex)] * halfHeight);
            g.drawVerticalLine(juce::roundToInt(x), juce::jmin(minY, maxY), juce::jmax(minY, maxY));
        }

        g.setColour(juce::Colour(0x70ffffff));
        g.drawHorizontalLine(centerY, static_cast<float>(waveformBounds.getX()), static_cast<float>(waveformBounds.getRight()));

        g.setColour(juce::Colour(0xfff6b64f));
        for (const auto& slice : stateSnapshot.visibleSlices)
        {
            if (slice.id < 0)
                continue;
            if (slice.normalizedEnd < visibleStart || slice.normalizedStart > visibleEnd)
                continue;

            const int x = waveformBounds.getX()
                        + juce::roundToInt(((slice.normalizedStart - visibleStart) / visibleWidth)
                                           * static_cast<float>(waveformBounds.getWidth()));
            g.drawVerticalLine(x, static_cast<float>(waveformBounds.getY() + 6), static_cast<float>(waveformBounds.getBottom() - 24));
            g.drawText(slice.label,
                       x + 3,
                       waveformBounds.getBottom() - 20,
                       32,
                       14,
                       juce::Justification::left,
                       false);
        }

        if (stateSnapshot.pendingVisibleSliceSlot >= 0
            && stateSnapshot.pendingVisibleSliceSlot < SliceModel::VisibleSliceCount)
        {
            const auto& pendingSlice = stateSnapshot.visibleSlices[static_cast<size_t>(stateSnapshot.pendingVisibleSliceSlot)];
            if (pendingSlice.id >= 0
                && pendingSlice.normalizedStart >= visibleStart
                && pendingSlice.normalizedStart <= visibleEnd)
            {
                const int x = waveformBounds.getX()
                            + juce::roundToInt(((pendingSlice.normalizedStart - visibleStart) / visibleWidth)
                                               * static_cast<float>(waveformBounds.getWidth()));
                g.setColour(juce::Colour(0x80ffffff));
                g.drawVerticalLine(x, static_cast<float>(waveformBounds.getY() + 3), static_cast<float>(waveformBounds.getBottom() - 3));
            }
        }

        for (size_t cueIndex = 0; cueIndex < stateSnapshot.cuePoints.size(); ++cueIndex)
        {
            const auto& cue = stateSnapshot.cuePoints[cueIndex];
            const float cueNorm = safeNormalizedPosition(cue.samplePosition, stateSnapshot.totalSamples);
            if (cueNorm < visibleStart || cueNorm > visibleEnd)
                continue;

            const int x = waveformBounds.getX()
                + juce::roundToInt(((cueNorm - visibleStart) / visibleWidth) * static_cast<float>(waveformBounds.getWidth()));
            const bool selected = static_cast<int>(cueIndex) == stateSnapshot.selectedCueIndex;
            g.setColour(selected ? juce::Colour(0xfff36f6f) : juce::Colour(0xffcfd7df));
            g.drawVerticalLine(x,
                               static_cast<float>(waveformBounds.getY() + 2),
                               static_cast<float>(waveformBounds.getBottom() - 2));
        }

        if (stateSnapshot.playbackProgress >= 0.0f)
        {
            if (stateSnapshot.playbackProgress >= visibleStart && stateSnapshot.playbackProgress <= visibleEnd)
            {
                const int playheadX = waveformBounds.getX()
                    + juce::roundToInt(((stateSnapshot.playbackProgress - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                g.setColour(juce::Colour(0xff8df0b8));
                g.drawVerticalLine(playheadX,
                                   static_cast<float>(waveformBounds.getY() + 2),
                                   static_cast<float>(waveformBounds.getBottom() - 2));
            }
        }
    }
    else
    {
        g.setColour(juce::Colour(0xff6b7787));
        g.drawFittedText("Async loader ready. Click Load, then trigger slices directly from this view.",
                         waveformBounds.reduced(12),
                         juce::Justification::centred,
                         2);
    }

    auto footer = bounds.reduced(2, 4);
    auto modeRow = footer.removeFromTop(22);
    sliceModeArea = modeRow.removeFromLeft(184);
    triggerModeArea = modeRow.removeFromLeft(104);
    auto infoBounds = footer.removeFromTop(18);
    leftNavBounds = infoBounds.removeFromLeft(34);
    rightNavBounds = infoBounds.removeFromRight(34);

    auto drawModeButton = [&](juce::Rectangle<int> area, const juce::String& text, bool active)
    {
        g.setColour(active ? juce::Colour(0xfff6b64f) : juce::Colour(0xff202833));
        g.fillRoundedRectangle(area.toFloat(), 5.0f);
        g.setColour(active ? juce::Colour(0xff1a1f24) : juce::Colour(0xff8d9aab));
        g.drawText(text, area, juce::Justification::centred);
    };

    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Uniform), "U", stateSnapshot.sliceMode == SampleSliceMode::Uniform);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Transient), "TR", stateSnapshot.sliceMode == SampleSliceMode::Transient);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Beat),
                   "B" + juce::String(stateSnapshot.beatDivision),
                   stateSnapshot.sliceMode == SampleSliceMode::Beat);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Manual), "M", stateSnapshot.sliceMode == SampleSliceMode::Manual);
    drawModeButton(getTriggerModeButtonBounds(SampleTriggerMode::OneShot), "1", stateSnapshot.triggerMode == SampleTriggerMode::OneShot);
    drawModeButton(getTriggerModeButtonBounds(SampleTriggerMode::Loop), "LP", stateSnapshot.triggerMode == SampleTriggerMode::Loop);

    g.setColour(stateSnapshot.canNavigateLeft ? juce::Colour(0xffd7dce2) : juce::Colour(0xff4d5968));
    g.drawFittedText("<", leftNavBounds, juce::Justification::centred, 1);
    g.setColour(stateSnapshot.canNavigateRight ? juce::Colour(0xffd7dce2) : juce::Colour(0xff4d5968));
    g.drawFittedText(">", rightNavBounds, juce::Justification::centred, 1);

    g.setColour(juce::Colour(0xff8d9aab));
    const auto bankText = "Slice bank "
        + juce::String(stateSnapshot.visibleSliceBankIndex + 1)
        + "/"
        + juce::String(juce::jmax(1, stateSnapshot.visibleSliceBankCount));
    g.drawText(bankText, infoBounds.removeFromLeft(160), juce::Justification::centredLeft);

    if (stateSnapshot.hasSample)
    {
        const auto seconds = stateSnapshot.sourceSampleRate > 0.0
            ? static_cast<double>(stateSnapshot.totalSamples) / stateSnapshot.sourceSampleRate
            : 0.0;
        g.drawText("Zoom " + juce::String(stateSnapshot.viewZoom, 1)
                   + "x  |  "
                   + juce::String(seconds, 2) + " s",
                   infoBounds,
                   juce::Justification::centredRight);
    }
}

void SampleModeComponent::resized()
{
}

void SampleModeComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto point = event.getPosition();
    draggingCueIndex = -1;
    createdCueOnMouseDown = false;

    for (const auto mode : { SampleSliceMode::Uniform, SampleSliceMode::Transient, SampleSliceMode::Beat, SampleSliceMode::Manual })
    {
        if (getSliceModeButtonBounds(mode).contains(point))
        {
            if (engine != nullptr)
            {
                if (mode == SampleSliceMode::Beat && stateSnapshot.sliceMode == SampleSliceMode::Beat)
                {
                    const int nextDivision = (stateSnapshot.beatDivision >= 8)
                        ? 1
                        : (stateSnapshot.beatDivision * 2);
                    engine->setBeatDivision(nextDivision);
                }
                else
                {
                    engine->setSliceMode(mode);
                }
            }
            return;
        }
    }

    for (const auto mode : { SampleTriggerMode::OneShot, SampleTriggerMode::Loop })
    {
        if (getTriggerModeButtonBounds(mode).contains(point))
        {
            if (engine != nullptr)
                engine->setTriggerMode(mode);
            return;
        }
    }

    if (leftNavBounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateLeft)
            onNavigateVisibleBank(-1);
        return;
    }

    if (rightNavBounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateRight)
            onNavigateVisibleBank(1);
        return;
    }

    if (engine != nullptr && stateSnapshot.sliceMode == SampleSliceMode::Manual && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        const int cueIndex = hitTestCuePoint(point);
        if (event.mods.isPopupMenu())
        {
            if (cueIndex >= 0)
                engine->deleteCuePoint(cueIndex);
            return;
        }

        if (cueIndex >= 0)
        {
            engine->selectCuePoint(cueIndex);
            draggingCueIndex = cueIndex;
            return;
        }

        draggingCueIndex = engine->createCuePointAtNormalizedPosition(normalizedPositionFromPoint(point));
        createdCueOnMouseDown = (draggingCueIndex >= 0);
        return;
    }

    const int visibleSlot = hitTestVisibleSlice(point);
    if (visibleSlot >= 0 && onTriggerVisibleSlice)
        onTriggerVisibleSlice(visibleSlot);
}

void SampleModeComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (engine == nullptr || draggingCueIndex < 0 || stateSnapshot.sliceMode != SampleSliceMode::Manual)
        return;

    engine->moveCuePoint(draggingCueIndex, normalizedPositionFromPoint(event.getPosition()));
}

void SampleModeComponent::mouseUp(const juce::MouseEvent&)
{
    draggingCueIndex = -1;
    createdCueOnMouseDown = false;
}

void SampleModeComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (engine == nullptr || !waveformBounds.contains(event.getPosition()))
        return;

    const bool scrollGesture = event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
    const float currentZoom = stateSnapshot.viewZoom;
    const float currentScroll = stateSnapshot.viewScroll;

    if (scrollGesture)
    {
        const float delta = (std::abs(wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY) * 0.08f;
        engine->setViewWindow(currentScroll - delta, currentZoom);
        return;
    }

    const float nextZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, currentZoom + (wheel.deltaY * 3.0f));
    engine->setViewWindow(currentScroll, nextZoom);
}

bool SampleModeComponent::keyPressed(const juce::KeyPress& key)
{
    if (engine == nullptr || stateSnapshot.sliceMode != SampleSliceMode::Manual)
        return false;

    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
        return engine->deleteCuePoint(stateSnapshot.selectedCueIndex);

    return false;
}

void SampleModeComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == engine)
        refreshFromEngine();
}

void SampleModeComponent::refreshFromEngine()
{
    if (engine == nullptr)
    {
        stateSnapshot = {};
        loadedSample.reset();
        repaint();
        return;
    }

    stateSnapshot = engine->getStateSnapshot();
    loadedSample = engine->getLoadedSample();
    repaint();
}

int SampleModeComponent::hitTestVisibleSlice(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point))
        return -1;

    const float normalizedX = normalizedPositionFromPoint(point);

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(slot)];
        if (slice.id < 0)
            continue;
        const bool isLastSlice = (slot == (SliceModel::VisibleSliceCount - 1));
        if (normalizedX >= slice.normalizedStart
            && (normalizedX < slice.normalizedEnd || (isLastSlice && normalizedX <= slice.normalizedEnd)))
            return slot;
    }

    return -1;
}

int SampleModeComponent::hitTestCuePoint(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleRange();
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < stateSnapshot.cuePoints.size(); ++i)
    {
        const float cueNorm = safeNormalizedPosition(stateSnapshot.cuePoints[i].samplePosition, stateSnapshot.totalSamples);
        if (cueNorm < visibleRange.getStart() || cueNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((cueNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kCueHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

float SampleModeComponent::normalizedPositionFromPoint(const juce::Point<int>& point) const
{
    const auto visibleRange = getVisibleRange();
    const int relativeX = point.x - waveformBounds.getX();
    const float xNorm = juce::jlimit(0.0f,
                                     1.0f,
                                     static_cast<float>(relativeX)
                                         / juce::jmax(1.0f, static_cast<float>(waveformBounds.getWidth())));
    return juce::jlimit(0.0f, 1.0f, visibleRange.getStart() + (xNorm * visibleRange.getLength()));
}

juce::Range<float> SampleModeComponent::getVisibleRange() const
{
    const float zoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, stateSnapshot.viewZoom);
    const float visibleLength = 1.0f / zoom;
    const float start = juce::jlimit(0.0f, juce::jmax(0.0f, 1.0f - visibleLength), stateSnapshot.viewScroll);
    return { start, juce::jlimit(0.0f, 1.0f, start + visibleLength) };
}

juce::Rectangle<int> SampleModeComponent::getSliceModeButtonBounds(SampleSliceMode mode) const
{
    const int index = static_cast<int>(mode);
    const int buttonWidth = 40;
    return { sliceModeArea.getX() + (index * (buttonWidth + 4)),
             sliceModeArea.getY(),
             buttonWidth,
             sliceModeArea.getHeight() };
}

juce::Rectangle<int> SampleModeComponent::getTriggerModeButtonBounds(SampleTriggerMode mode) const
{
    const int index = static_cast<int>(mode);
    const int buttonWidth = 46;
    return { triggerModeArea.getX() + (index * (buttonWidth + 4)),
             triggerModeArea.getY(),
             buttonWidth,
             triggerModeArea.getHeight() };
}

void SampleModeComponent::timerCallback()
{
    if (engine != nullptr)
        refreshFromEngine();
}
