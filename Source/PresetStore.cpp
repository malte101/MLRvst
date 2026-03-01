#include "PresetStore.h"
#include "AudioEngine.h"
#include "PlayheadSpeedQuantizer.h"
#include <cmath>
#include <limits>

namespace PresetStore
{
static constexpr int kMaxPresetSlots = 16 * 7;

namespace
{
constexpr const char* kEmbeddedSampleAttr = "embeddedSampleWavBase64";
constexpr const char* kAnalysisTransientAttr = "analysisTransientSlices";
constexpr const char* kAnalysisRmsAttr = "analysisRmsMap";
constexpr const char* kAnalysisZeroCrossAttr = "analysisZeroCrossMap";
constexpr const char* kAnalysisSampleCountAttr = "analysisSampleCount";
constexpr int kMaxEmbeddedBase64Chars = 64 * 1024 * 1024;
constexpr size_t kMaxEmbeddedWavBytes = 48 * 1024 * 1024;
constexpr int64_t kMaxPresetXmlBytes = 128LL * 1024LL * 1024LL;
constexpr int64_t kMaxPresetNameXmlBytes = 8LL * 1024LL * 1024LL;
constexpr int kMaxStoredSamplePathChars = 4096;

struct GlobalParameterSnapshot
{
    float masterVolume = 1.0f;
    float limiterThresholdDb = 0.0f;
    float limiterEnabled = 0.0f;
    float quantizeChoice = 5.0f;
    float innerLoopLengthChoice = 0.0f;
    float grainQuality = 2.0f;
    float pitchSmoothing = 0.05f;
    float inputMonitor = 1.0f;
    float crossfadeMs = 10.0f;
    float triggerFadeInMs = 12.0f;
    float outputRouting = 0.0f;
    float pitchControlMode = 0.0f;
};

bool isPresetFileSizeValid(const juce::File& file, int64_t maxBytes)
{
    if (!file.existsAsFile())
        return false;

    const int64_t size = file.getSize();
    return size > 0 && size <= maxBytes;
}

std::unique_ptr<juce::XmlElement> parsePresetXmlSafely(const juce::File& presetFile, int64_t maxBytes)
{
    if (!isPresetFileSizeValid(presetFile, maxBytes))
        return nullptr;

    auto xml = juce::XmlDocument::parse(presetFile);
    if (xml == nullptr || !xml->hasTagName("mlrVSTPreset"))
        return nullptr;

    return xml;
}

bool writePresetAtomically(const juce::XmlElement& preset, const juce::File& targetFile)
{
    juce::TemporaryFile tempFile(targetFile);
    if (!preset.writeTo(tempFile.getFile()))
        return false;

    return tempFile.overwriteTargetFileWithTemporary();
}

bool isValidStoredSamplePath(const juce::String& rawPath)
{
    const auto path = rawPath.trim();
    if (path.isEmpty() || path.length() > kMaxStoredSamplePathChars)
        return false;

    if (!juce::File::isAbsolutePath(path))
        return false;

    if (path.contains("\n") || path.contains("\r") || path.contains("://"))
        return false;

    if (path.startsWith("//") || path.startsWith("\\\\"))
        return false;

    const juce::File file(path);
    if (!file.hasFileExtension("wav;aif;aiff;mp3;ogg;flac"))
        return false;

    return true;
}

bool shouldEmbedAudioBuffer(const juce::AudioBuffer<float>& buffer)
{
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return false;

    const uint64_t estimatedWavBytes = static_cast<uint64_t>(channels)
        * static_cast<uint64_t>(samples)
        * 3ULL  // 24-bit WAV payload
        + 4096ULL;
    return estimatedWavBytes <= static_cast<uint64_t>(kMaxEmbeddedWavBytes);
}

GlobalParameterSnapshot captureGlobalParameters(juce::AudioProcessorValueTreeState& parameters)
{
    GlobalParameterSnapshot snapshot;
    if (auto* p = parameters.getRawParameterValue("masterVolume"))
        snapshot.masterVolume = *p;
    if (auto* p = parameters.getRawParameterValue("limiterThreshold"))
        snapshot.limiterThresholdDb = *p;
    if (auto* p = parameters.getRawParameterValue("limiterEnabled"))
        snapshot.limiterEnabled = *p;
    if (auto* p = parameters.getRawParameterValue("quantize"))
        snapshot.quantizeChoice = *p;
    if (auto* p = parameters.getRawParameterValue("innerLoopLength"))
        snapshot.innerLoopLengthChoice = *p;
    if (auto* p = parameters.getRawParameterValue("quality"))
        snapshot.grainQuality = *p;
    if (auto* p = parameters.getRawParameterValue("pitchSmoothing"))
        snapshot.pitchSmoothing = *p;
    if (auto* p = parameters.getRawParameterValue("inputMonitor"))
        snapshot.inputMonitor = *p;
    if (auto* p = parameters.getRawParameterValue("crossfadeLength"))
        snapshot.crossfadeMs = *p;
    if (auto* p = parameters.getRawParameterValue("triggerFadeIn"))
        snapshot.triggerFadeInMs = *p;
    if (auto* p = parameters.getRawParameterValue("outputRouting"))
        snapshot.outputRouting = *p;
    if (auto* p = parameters.getRawParameterValue("pitchControlMode"))
        snapshot.pitchControlMode = *p;
    return snapshot;
}

void restoreGlobalParameters(juce::AudioProcessorValueTreeState& parameters, const GlobalParameterSnapshot& snapshot)
{
    if (auto* param = parameters.getParameter("masterVolume"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.masterVolume));
    if (auto* param = parameters.getParameter("limiterThreshold"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (snapshot.limiterThresholdDb + 24.0f) / 24.0f));
    if (auto* param = parameters.getParameter("limiterEnabled"))
        param->setValueNotifyingHost(snapshot.limiterEnabled > 0.5f ? 1.0f : 0.0f);
    if (auto* param = parameters.getParameter("quantize"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.quantizeChoice / 9.0f));
    if (auto* param = parameters.getParameter("innerLoopLength"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.innerLoopLengthChoice / 4.0f));
    if (auto* param = parameters.getParameter("quality"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.grainQuality / 3.0f));
    if (auto* param = parameters.getParameter("pitchSmoothing"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.pitchSmoothing));
    if (auto* param = parameters.getParameter("inputMonitor"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.inputMonitor));
    if (auto* param = parameters.getParameter("crossfadeLength"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (snapshot.crossfadeMs - 1.0f) / 49.0f));
    if (auto* param = parameters.getParameter("triggerFadeIn"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (snapshot.triggerFadeInMs - 0.1f) / 119.9f));
    if (auto* param = parameters.getParameter("outputRouting"))
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, ranged->convertTo0to1(snapshot.outputRouting)));
        else
            param->setValueNotifyingHost(snapshot.outputRouting > 0.5f ? 1.0f : 0.0f);
    }
    if (auto* param = parameters.getParameter("pitchControlMode"))
        param->setValueNotifyingHost(snapshot.pitchControlMode > 0.5f ? 1.0f : 0.0f);
}

void setParameterToDefault(juce::AudioProcessorValueTreeState& parameters, const juce::String& parameterId)
{
    if (auto* param = parameters.getParameter(parameterId))
        param->setValueNotifyingHost(param->getDefaultValue());
}

void resetStripParametersToDefaults(juce::AudioProcessorValueTreeState& parameters, int stripIndex)
{
    setParameterToDefault(parameters, "stripVolume" + juce::String(stripIndex));
    setParameterToDefault(parameters, "stripPan" + juce::String(stripIndex));
    setParameterToDefault(parameters, "stripSpeed" + juce::String(stripIndex));
    setParameterToDefault(parameters, "stripPitch" + juce::String(stripIndex));
    setParameterToDefault(parameters, "stripSliceLength" + juce::String(stripIndex));
}

void resetStripToDefaultState(int stripIndex,
                              EnhancedAudioStrip& strip,
                              ModernAudioEngine& audioEngine,
                              juce::AudioProcessorValueTreeState& parameters)
{
    strip.clearSample();
    strip.stop(true);
    strip.setLoop(0, ModernAudioEngine::MaxColumns);
    strip.setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
    strip.setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
    strip.setReverse(false);
    strip.setVolume(1.0f);
    strip.setPan(0.0f);
    strip.setPlaybackSpeed(1.0f);
    strip.setBeatsPerLoop(-1.0f);
    strip.setScratchAmount(0.0f);
    strip.setTransientSliceMode(false);
    strip.setLoopSliceLength(1.0f);
    strip.setResamplePitchEnabled(false);
    strip.setResamplePitchRatio(1.0f);
    strip.setPitchShift(0.0f);
    strip.setRecordingBars(1);
    strip.setFilterFrequency(20000.0f);
    strip.setFilterResonance(0.707f);
    strip.setFilterMorph(0.0f);
    strip.setFilterAlgorithm(EnhancedAudioStrip::FilterAlgorithm::Tpt12);
    strip.setFilterEnabled(false);
    strip.setSwingAmount(0.0f);
    strip.setGateAmount(0.0f);
    strip.setGateSpeed(4.0f);
    strip.setGateEnvelope(0.5f);
    strip.setGateShape(0.5f);
    strip.setStepPatternBars(1);
    strip.setStepPage(0);
    strip.currentStep = 0;
    strip.stepPattern.fill(false);
    strip.stepSubdivisions.fill(1);
    strip.stepSubdivisionStartVelocity.fill(1.0f);
    strip.stepSubdivisionRepeatVelocity.fill(1.0f);
    strip.stepProbability.fill(1.0f);
    strip.setStepEnvelopeAttackMs(0.0f);
    strip.setStepEnvelopeDecayMs(4000.0f);
    strip.setStepEnvelopeReleaseMs(110.0f);
    strip.setGrainSizeMs(1240.0f);
    strip.setGrainDensity(0.05f);
    strip.setGrainPitch(0.0f);
    strip.setGrainPitchJitter(0.0f);
    strip.setGrainSpread(0.0f);
    strip.setGrainJitter(0.0f);
    strip.setGrainPositionJitter(0.0f);
    strip.setGrainRandomDepth(0.0f);
    strip.setGrainArpDepth(0.0f);
    strip.setGrainCloudDepth(0.0f);
    strip.setGrainEmitterDepth(0.0f);
    strip.setGrainEnvelope(0.0f);
    strip.setGrainShape(0.0f);
    strip.setGrainArpMode(0);
    strip.setGrainTempoSyncEnabled(true);

    audioEngine.assignStripToGroup(stripIndex, -1);
    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        audioEngine.setModSequencerSlot(stripIndex, slot);
        audioEngine.setModTarget(stripIndex, ModernAudioEngine::ModTarget::None);
        audioEngine.setModBipolar(stripIndex, false);
        audioEngine.setModCurveMode(stripIndex, false);
        audioEngine.setModDepth(stripIndex, 1.0f);
        audioEngine.setModOffset(stripIndex, 0);
        audioEngine.setModLengthBars(stripIndex, 1);
        audioEngine.setModEditPage(stripIndex, 0);
        audioEngine.setModSmoothingMs(stripIndex, 0.0f);
        audioEngine.setModCurveBend(stripIndex, 0.0f);
        audioEngine.setModCurveShape(stripIndex, ModernAudioEngine::ModCurveShape::Linear);
        audioEngine.setModPitchScaleQuantize(stripIndex, false);
        audioEngine.setModPitchScale(stripIndex, ModernAudioEngine::PitchScale::Chromatic);
        for (int s = 0; s < ModernAudioEngine::ModTotalSteps; ++s)
            audioEngine.setModStepValueAbsolute(stripIndex, s, 0.0f);
    }
    audioEngine.setModSequencerSlot(stripIndex, 0);

    resetStripParametersToDefaults(parameters, stripIndex);
}

juce::String encodeStepPatternBits(const std::array<bool, 64>& bits)
{
    juce::String out;
    out.preallocateBytes(64);
    for (bool b : bits)
        out += (b ? "1" : "0");
    return out;
}

void decodeStepPatternBits(const juce::String& text, std::array<bool, 64>& bits)
{
    bits.fill(false);
    const int len = juce::jmin(64, text.length());
    for (int i = 0; i < len; ++i)
        bits[static_cast<size_t>(i)] = (text[i] == '1');
}

juce::String encodeStepSubdivisions(const std::array<int, 64>& subdivisions)
{
    juce::String out;
    out.preallocateBytes(64 * 3);
    for (size_t i = 0; i < subdivisions.size(); ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::jlimit(1, EnhancedAudioStrip::MaxStepSubdivisions, subdivisions[i]);
    }
    return out;
}

void decodeStepSubdivisions(const juce::String& text, std::array<int, 64>& subdivisions)
{
    subdivisions.fill(1);
    if (text.isEmpty())
        return;

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int len = juce::jmin(static_cast<int>(subdivisions.size()), tokens.size());
    for (int i = 0; i < len; ++i)
        subdivisions[static_cast<size_t>(i)] = juce::jlimit(
            1, EnhancedAudioStrip::MaxStepSubdivisions, tokens[i].getIntValue());
}

juce::String encodeStepSubdivisionRepeatVelocity(const std::array<float, 64>& repeatVelocity)
{
    juce::String out;
    out.preallocateBytes(64 * 8);
    for (size_t i = 0; i < repeatVelocity.size(); ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::String(juce::jlimit(0.0f, 1.0f, repeatVelocity[i]), 5);
    }
    return out;
}

void decodeStepSubdivisionRepeatVelocity(const juce::String& text, std::array<float, 64>& repeatVelocity)
{
    repeatVelocity.fill(1.0f);
    if (text.isEmpty())
        return;

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int len = juce::jmin(static_cast<int>(repeatVelocity.size()), tokens.size());
    for (int i = 0; i < len; ++i)
        repeatVelocity[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, tokens[i].getFloatValue());
}

juce::String encodeModSteps(const std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    juce::String out;
    out.preallocateBytes(ModernAudioEngine::ModTotalSteps * 8);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::String(juce::jlimit(0.0f, 1.0f, steps[i]), 6);
    }
    return out;
}

void decodeModStepsLegacyBits(const juce::String& text, std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    steps.fill(0.0f);
    const int len = juce::jmin(ModernAudioEngine::ModSteps, text.length());
    for (int i = 0; i < len; ++i)
        steps[static_cast<size_t>(i)] = (text[i] == '1') ? 1.0f : 0.0f;
}

void decodeModSteps(const juce::String& text, std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    steps.fill(0.0f);
    if (!text.containsChar(','))
    {
        decodeModStepsLegacyBits(text, steps);
        return;
    }

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int len = juce::jmin(ModernAudioEngine::ModTotalSteps, tokens.size());
    for (int i = 0; i < len; ++i)
        steps[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, static_cast<float>(tokens[i].getDoubleValue()));
}

template <size_t N>
juce::String encodeIntArrayCsv(const std::array<int, N>& values)
{
    juce::String out;
    out.preallocateBytes(static_cast<int>(N * 8));
    for (size_t i = 0; i < N; ++i)
    {
        if (i > 0)
            out << ",";
        out << values[i];
    }
    return out;
}

template <size_t N>
juce::String encodeFloatArrayCsv(const std::array<float, N>& values)
{
    juce::String out;
    out.preallocateBytes(static_cast<int>(N * 8));
    for (size_t i = 0; i < N; ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::String(values[i], 6);
    }
    return out;
}

template <size_t N>
void decodeIntArrayCsv(const juce::String& csvText, std::array<int, N>& outValues)
{
    juce::StringArray tokens;
    tokens.addTokens(csvText, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    for (size_t i = 0; i < N; ++i)
    {
        if (static_cast<int>(i) < tokens.size())
            outValues[i] = tokens[static_cast<int>(i)].getIntValue();
    }
}

template <size_t N>
void decodeFloatArrayCsv(const juce::String& csvText, std::array<float, N>& outValues)
{
    juce::StringArray tokens;
    tokens.addTokens(csvText, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    for (size_t i = 0; i < N; ++i)
    {
        if (static_cast<int>(i) < tokens.size())
            outValues[i] = tokens[static_cast<int>(i)].getFloatValue();
    }
}

bool writeDefaultPresetFile(const juce::File& presetFile, int presetIndex)
{
    juce::XmlElement preset("mlrVSTPreset");
    preset.setAttribute("version", "1.0");
    preset.setAttribute("index", presetIndex);
    if (presetFile.existsAsFile())
    {
        if (auto existing = parsePresetXmlSafely(presetFile, kMaxPresetNameXmlBytes))
        {
            const auto existingName = existing->getStringAttribute("name").trim();
            if (existingName.isNotEmpty())
                preset.setAttribute("name", existingName);
        }
    }

    auto* globalsXml = preset.createNewChildElement("Globals");
    globalsXml->setAttribute("masterVolume", 0.7);
    globalsXml->setAttribute("quantize", 5);
    globalsXml->setAttribute("innerLoopLength", 0);
    globalsXml->setAttribute("crossfadeLength", 10.0);

    return writePresetAtomically(preset, presetFile);
}

bool encodeBufferAsWavBase64(const juce::AudioBuffer<float>& buffer,
                             double sampleRate,
                             juce::String& outBase64)
{
    outBase64.clear();

    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0
        || !std::isfinite(sampleRate) || sampleRate <= 1000.0)
        return false;

    auto wavBytes = std::make_unique<juce::MemoryOutputStream>();
    auto* wavBytesRaw = wavBytes.get();
    juce::WavAudioFormat wavFormat;
    auto writerStream = std::unique_ptr<juce::OutputStream>(wavBytes.release());
    const auto writerOptions = juce::AudioFormatWriter::Options{}
        .withSampleRate(sampleRate)
        .withNumChannels(buffer.getNumChannels())
        .withBitsPerSample(24)
        .withQualityOptionIndex(0);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(writerStream, writerOptions));

    if (!writer)
        return false;

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
        return false;

    writer->flush();

    const auto data = wavBytesRaw->getMemoryBlock();
    outBase64 = data.toBase64Encoding();
    writer.reset();
    return outBase64.isNotEmpty();
}

bool decodeWavBase64ToStrip(const juce::String& base64Data, EnhancedAudioStrip& strip)
{
    if (base64Data.isEmpty() || base64Data.length() > kMaxEmbeddedBase64Chars)
        return false;

    juce::MemoryBlock wavBytes;
    if (!wavBytes.fromBase64Encoding(base64Data) || wavBytes.getSize() == 0)
        return false;
    if (wavBytes.getSize() > kMaxEmbeddedWavBytes)
        return false;

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader(
        wavFormat.createReaderFor(new juce::MemoryInputStream(wavBytes.getData(), wavBytes.getSize(), false), true));
    if (!reader)
        return false;

    const int64_t totalSamples64 = reader->lengthInSamples;
    if (totalSamples64 <= 0 || totalSamples64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return false;

    const int totalSamples = static_cast<int>(totalSamples64);
    const int channelCount = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> buffer(channelCount, totalSamples);

    if (!reader->read(&buffer, 0, totalSamples, 0, true, true))
        return false;

    strip.loadSample(buffer, reader->sampleRate);
    return strip.hasAudio();
}
}

juce::File getPresetDirectory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("Presets");
    if (!dir.exists())
        dir.createDirectory();
    return dir;
}

bool savePreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const juce::File* currentStripFiles)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots || audioEngine == nullptr || currentStripFiles == nullptr)
        return false;

    try
    {
        auto presetDir = getPresetDirectory();
        if (!presetDir.exists())
            presetDir.createDirectory();

        auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");

        juce::XmlElement preset("mlrVSTPreset");
        preset.setAttribute("version", "1.0");
        preset.setAttribute("index", presetIndex);
        if (presetFile.existsAsFile())
        {
            if (auto existing = parsePresetXmlSafely(presetFile, kMaxPresetNameXmlBytes))
            {
                const auto existingName = existing->getStringAttribute("name").trim();
                if (existingName.isNotEmpty())
                    preset.setAttribute("name", existingName);
            }
        }

    for (int i = 0; i < maxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (strip == nullptr)
            continue;

        auto* stripXml = preset.createNewChildElement("Strip");
        stripXml->setAttribute("index", i);

        if (strip->hasAudio())
        {
            const juce::String storedPath = currentStripFiles[i].getFullPathName().trim();
            if (isValidStoredSamplePath(storedPath))
            {
                stripXml->setAttribute("samplePath", storedPath);
            }
            else if (const auto* audioBuffer = strip->getAudioBuffer())
            {
                juce::String embeddedWav;
                if (shouldEmbedAudioBuffer(*audioBuffer)
                    && encodeBufferAsWavBase64(*audioBuffer, strip->getSourceSampleRate(), embeddedWav))
                {
                    stripXml->setAttribute(kEmbeddedSampleAttr, embeddedWav);
                }
                else
                {
                    DBG("Preset save strip " << i << ": skipped embedded sample (invalid path or embed too large)");
                }
            }
        }

        stripXml->setAttribute("volume", strip->getVolume());
        stripXml->setAttribute("pan", strip->getPan());
        const float savedSpeedRatio = PlayheadSpeedQuantizer::quantizeRatio(strip->getPlayheadSpeedRatio());
        stripXml->setAttribute("speed", savedSpeedRatio);
        stripXml->setAttribute("loopStart", strip->getLoopStart());
        stripXml->setAttribute("loopEnd", strip->getLoopEnd());
        stripXml->setAttribute("playMode", static_cast<int>(strip->getPlayMode()));
        stripXml->setAttribute("isPlaying", strip->isPlaying());
        stripXml->setAttribute("playbackColumn", strip->getCurrentColumn());
        stripXml->setAttribute("ppqTimelineAnchored", strip->isPpqTimelineAnchored());
        stripXml->setAttribute("ppqTimelineOffsetBeats", strip->getPpqTimelineOffsetBeats());
        stripXml->setAttribute("directionMode", static_cast<int>(strip->getDirectionMode()));
        stripXml->setAttribute("reversed", strip->isReversed());
        stripXml->setAttribute("group", strip->getGroup());
        stripXml->setAttribute("beatsPerLoop", strip->getBeatsPerLoop());
        stripXml->setAttribute("scratchAmount", strip->getScratchAmount());
        stripXml->setAttribute("transientSliceMode", strip->isTransientSliceMode());
        stripXml->setAttribute("loopSliceLength", strip->getLoopSliceLength());
        if (strip->hasSampleAnalysisCache())
        {
            stripXml->setAttribute(kAnalysisSampleCountAttr, strip->getAnalysisSampleCount());
            stripXml->setAttribute(kAnalysisTransientAttr, encodeIntArrayCsv(strip->getCachedTransientSliceSamples()));
            stripXml->setAttribute(kAnalysisRmsAttr, encodeFloatArrayCsv(strip->getCachedRmsMap()));
            stripXml->setAttribute(kAnalysisZeroCrossAttr, encodeIntArrayCsv(strip->getCachedZeroCrossMap()));
        }
        stripXml->setAttribute("pitchShift", strip->getPitchShift());
        stripXml->setAttribute("recordingBars", strip->getRecordingBars());
        stripXml->setAttribute("filterEnabled", strip->isFilterEnabled());
        stripXml->setAttribute("filterFrequency", strip->getFilterFrequency());
        stripXml->setAttribute("filterResonance", strip->getFilterResonance());
        stripXml->setAttribute("filterMorph", strip->getFilterMorph());
        stripXml->setAttribute("filterAlgorithm", static_cast<int>(strip->getFilterAlgorithm()));
        stripXml->setAttribute("filterType", static_cast<int>(strip->getFilterType()));
        stripXml->setAttribute("swingAmount", strip->getSwingAmount());
        stripXml->setAttribute("gateAmount", strip->getGateAmount());
        stripXml->setAttribute("gateSpeed", strip->getGateSpeed());
        stripXml->setAttribute("gateEnvelope", strip->getGateEnvelope());
        stripXml->setAttribute("gateShapeCurve", strip->getGateShape());
        stripXml->setAttribute("stepPatternSteps", strip->getStepPatternLengthSteps());
        stripXml->setAttribute("stepPatternBars", strip->getStepPatternBars());
        stripXml->setAttribute("stepViewPage", strip->getStepPage());
        stripXml->setAttribute("stepCurrent", strip->currentStep);
        stripXml->setAttribute("stepPatternBits", encodeStepPatternBits(strip->stepPattern));
        stripXml->setAttribute("stepSubdivisions", encodeStepSubdivisions(strip->stepSubdivisions));
        stripXml->setAttribute("stepSubdivisionStartVelocity",
                               encodeStepSubdivisionRepeatVelocity(strip->stepSubdivisionStartVelocity));
        stripXml->setAttribute("stepSubdivisionRepeatVelocity",
                               encodeStepSubdivisionRepeatVelocity(strip->stepSubdivisionRepeatVelocity));
        stripXml->setAttribute("stepProbability",
                               encodeStepSubdivisionRepeatVelocity(strip->stepProbability));
        stripXml->setAttribute("stepAttackMs", strip->getStepEnvelopeAttackMs());
        stripXml->setAttribute("stepDecayMs", strip->getStepEnvelopeDecayMs());
        stripXml->setAttribute("stepReleaseMs", strip->getStepEnvelopeReleaseMs());

        stripXml->setAttribute("grainSizeMs", strip->getGrainSizeMs());
        stripXml->setAttribute("grainDensity", strip->getGrainDensity());
        stripXml->setAttribute("grainPitch", strip->getGrainPitch());
        stripXml->setAttribute("grainPitchJitter", strip->getGrainPitchJitter());
        stripXml->setAttribute("grainSpread", strip->getGrainSpread());
        stripXml->setAttribute("grainJitter", strip->getGrainJitter());
        stripXml->setAttribute("grainPositionJitter", strip->getGrainPositionJitter());
        stripXml->setAttribute("grainRandomDepth", strip->getGrainRandomDepth());
        stripXml->setAttribute("grainArpDepth", strip->getGrainArpDepth());
        stripXml->setAttribute("grainCloudDepth", strip->getGrainCloudDepth());
        stripXml->setAttribute("grainEmitterDepth", strip->getGrainEmitterDepth());
        stripXml->setAttribute("grainEnvelope", strip->getGrainEnvelope());
        stripXml->setAttribute("grainShape", strip->getGrainShape());
        stripXml->setAttribute("grainArpMode", strip->getGrainArpMode());
        stripXml->setAttribute("grainTempoSync", strip->isGrainTempoSyncEnabled());

        const int originalModSlot = audioEngine->getModSequencerSlot(i);
        stripXml->setAttribute("modActiveSequencer", originalModSlot);
        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            audioEngine->setModSequencerSlot(i, modSlot);
            const auto mod = audioEngine->getModSequencerState(i);
            std::array<float, ModernAudioEngine::ModTotalSteps> modSteps{};
            std::array<int, ModernAudioEngine::ModTotalSteps> modStepSubdivisions{};
            std::array<float, ModernAudioEngine::ModTotalSteps> modStepEndValues{};
            std::array<int, ModernAudioEngine::ModTotalSteps> modStepCurveShapes{};
            for (int s = 0; s < ModernAudioEngine::ModTotalSteps; ++s)
            {
                modSteps[static_cast<size_t>(s)] = audioEngine->getModStepValueAbsolute(i, s);
                modStepSubdivisions[static_cast<size_t>(s)] = juce::jlimit(
                    1, ModernAudioEngine::ModMaxStepSubdivisions, audioEngine->getModStepSubdivisionAbsolute(i, s));
                modStepEndValues[static_cast<size_t>(s)] = juce::jlimit(
                    0.0f, 1.0f, audioEngine->getModStepEndValueAbsolute(i, s));
                modStepCurveShapes[static_cast<size_t>(s)] = juce::jlimit(
                    0,
                    static_cast<int>(ModernAudioEngine::ModCurveShape::Square),
                    static_cast<int>(audioEngine->getModStepCurveShapeAbsolute(i, s)));
            }

            auto slotKey = [modSlot](const juce::String& suffix)
            {
                if (modSlot == 0)
                    return "mod" + suffix;
                return "mod" + juce::String(modSlot + 1) + suffix;
            };

            stripXml->setAttribute(slotKey("Target"), static_cast<int>(mod.target));
            stripXml->setAttribute(slotKey("Bipolar"), mod.bipolar);
            stripXml->setAttribute(slotKey("CurveMode"), mod.curveMode);
            stripXml->setAttribute(slotKey("Depth"), mod.depth);
            stripXml->setAttribute(slotKey("Offset"), mod.offset);
            stripXml->setAttribute(slotKey("LengthBars"), mod.lengthBars);
            stripXml->setAttribute(slotKey("EditPage"), mod.editPage);
            stripXml->setAttribute(slotKey("SmoothMs"), mod.smoothingMs);
            stripXml->setAttribute(slotKey("CurveBend"), mod.curveBend);
            stripXml->setAttribute(slotKey("CurveShape"), mod.curveShape);
            stripXml->setAttribute(slotKey("PitchScaleQuantize"), mod.pitchScaleQuantize);
            stripXml->setAttribute(slotKey("PitchScale"), mod.pitchScale);
            stripXml->setAttribute(slotKey("Steps"), encodeModSteps(modSteps));
            stripXml->setAttribute(slotKey("StepSubdivisions"), encodeIntArrayCsv(modStepSubdivisions));
            stripXml->setAttribute(slotKey("StepEndValues"), encodeFloatArrayCsv(modStepEndValues));
            stripXml->setAttribute(slotKey("StepCurveShapes"), encodeIntArrayCsv(modStepCurveShapes));
        }
        audioEngine->setModSequencerSlot(i, originalModSlot);
    }

    auto* groupsXml = preset.createNewChildElement("Groups");
    for (int i = 0; i < ModernAudioEngine::MaxGroups; ++i)
    {
        if (auto* group = audioEngine->getGroup(i))
        {
            auto* groupXml = groupsXml->createNewChildElement("Group");
            groupXml->setAttribute("index", i);
            groupXml->setAttribute("volume", group->getVolume());
            groupXml->setAttribute("muted", group->isMuted());
        }
    }

    auto* patternsXml = preset.createNewChildElement("Patterns");
    for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
    {
        if (auto* pattern = audioEngine->getPattern(i))
        {
            auto* patternXml = patternsXml->createNewChildElement("Pattern");
            patternXml->setAttribute("index", i);
            patternXml->setAttribute("lengthBeats", pattern->getLengthInBeats());
            patternXml->setAttribute("isPlaying", pattern->isPlaying());
            const auto events = pattern->getEventsSnapshot();
            for (const auto& e : events)
            {
                auto* eventXml = patternXml->createNewChildElement("Event");
                eventXml->setAttribute("strip", e.stripIndex);
                eventXml->setAttribute("column", e.column);
                eventXml->setAttribute("time", e.time);
                eventXml->setAttribute("noteOn", e.isNoteOn);
            }
        }
    }

    if (auto stateXml = parameters.copyState().createXml())
    {
        stateXml->setTagName("ParametersState");
        preset.addChildElement(stateXml.release());
    }

    auto* globalsXml = preset.createNewChildElement("Globals");
    if (auto* masterVol = parameters.getRawParameterValue("masterVolume"))
        globalsXml->setAttribute("masterVolume", *masterVol);
    if (auto* quantize = parameters.getRawParameterValue("quantize"))
        globalsXml->setAttribute("quantize", static_cast<int>(*quantize));
    if (auto* innerLoopLength = parameters.getRawParameterValue("innerLoopLength"))
        globalsXml->setAttribute("innerLoopLength", static_cast<int>(*innerLoopLength));
    if (auto* crossfade = parameters.getRawParameterValue("crossfadeLength"))
        globalsXml->setAttribute("crossfadeLength", *crossfade);

        if (writePresetAtomically(preset, presetFile))
        {
            DBG("Preset " << (presetIndex + 1) << " saved: " << presetFile.getFullPathName());
            return true;
        }

        DBG("Preset save failed for slot " << (presetIndex + 1) << ": write failed");
        return false;
    }
    catch (const std::exception& e)
    {
        DBG("Preset save failed for slot " << (presetIndex + 1) << ": " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("Preset save failed for slot " << (presetIndex + 1) << ": unknown exception");
        return false;
    }

    return false;
}

bool loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<bool(int, const juce::File&)>& loadSampleToStrip,
                double hostPpqSnapshot,
                double hostTempoSnapshot)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots || audioEngine == nullptr)
        return false;

    try
    {
        auto presetDir = getPresetDirectory();
        auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");

        if (!presetFile.existsAsFile())
        {
            if (writeDefaultPresetFile(presetFile, presetIndex))
                DBG("Preset " << (presetIndex + 1) << " missing - created default preset file");
            else
            {
                DBG("Preset " << (presetIndex + 1) << " not found and could not be created");
                return false;
            }
        }

        if (!isPresetFileSizeValid(presetFile, kMaxPresetXmlBytes))
        {
            DBG("Preset " << (presetIndex + 1) << " rejected (invalid file size)");
            return false;
        }

        auto preset = parsePresetXmlSafely(presetFile, kMaxPresetXmlBytes);
        if (!preset)
        {
            // Attempt self-heal for malformed files.
            if (!writeDefaultPresetFile(presetFile, presetIndex))
            {
                DBG("Invalid preset file and recovery failed");
                return false;
            }
            preset = parsePresetXmlSafely(presetFile, kMaxPresetXmlBytes);
            if (!preset)
            {
                DBG("Invalid preset file after recovery");
                return false;
            }
        }

        const auto globalSnapshot = captureGlobalParameters(parameters);

        bool restoredParameterState = false;
        if (auto* paramsXml = preset->getChildByName("ParametersState"))
        {
            auto state = juce::ValueTree::fromXml(*paramsXml);
            if (state.isValid())
            {
                parameters.replaceState(state);
                restoredParameterState = true;
            }
        }

        // Preset recall should not overwrite global controls.
        restoreGlobalParameters(parameters, globalSnapshot);

        const int safeMaxStrips = juce::jlimit(0, ModernAudioEngine::MaxStrips, maxStrips);
        if (!restoredParameterState)
        {
            for (int i = 0; i < safeMaxStrips; ++i)
                resetStripParametersToDefaults(parameters, i);
        }

        const bool canRecallPlayingState = std::isfinite(hostPpqSnapshot)
            && std::isfinite(hostTempoSnapshot)
            && hostTempoSnapshot > 0.0;
        const double recallPpq = std::isfinite(hostPpqSnapshot)
            ? hostPpqSnapshot
            : audioEngine->getTimelineBeat();
        const double recallTempo = (std::isfinite(hostTempoSnapshot) && hostTempoSnapshot > 0.0)
            ? hostTempoSnapshot
            : audioEngine->getCurrentTempo();

        std::vector<bool> stripSeen(static_cast<size_t>(safeMaxStrips), false);
        for (auto* stripXml : preset->getChildWithTagNameIterator("Strip"))
        {
        int stripIndex = stripXml->getIntAttribute("index");
        if (stripIndex < 0 || stripIndex >= safeMaxStrips)
            continue;

        stripSeen[static_cast<size_t>(stripIndex)] = true;
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const juce::String samplePath = stripXml->getStringAttribute("samplePath").trim();
        bool loadedStripAudio = false;
        if (samplePath.isNotEmpty() && isValidStoredSamplePath(samplePath))
        {
            juce::File sampleFile(samplePath);
            if (sampleFile.existsAsFile())
            {
                loadedStripAudio = loadSampleToStrip(stripIndex, sampleFile);
            }
        }

        if (!loadedStripAudio)
        {
            const juce::String embeddedSample = stripXml->getStringAttribute(kEmbeddedSampleAttr);
            if (embeddedSample.isNotEmpty())
                loadedStripAudio = decodeWavBase64ToStrip(embeddedSample, *strip);
        }

        if (!loadedStripAudio)
            strip->clearSample();

        auto finiteFloat = [](double value, float fallback)
        {
            return std::isfinite(value) ? static_cast<float>(value) : fallback;
        };
        auto clampedFloat = [&](double value, float fallback, float minV, float maxV)
        {
            return juce::jlimit(minV, maxV, finiteFloat(value, fallback));
        };
        auto clampedInt = [](int value, int minV, int maxV, int fallback)
        {
            if (value < minV || value > maxV)
                return fallback;
            return value;
        };

        strip->setVolume(clampedFloat(stripXml->getDoubleAttribute("volume", 1.0), 1.0f, 0.0f, 1.0f));
        strip->setPan(clampedFloat(stripXml->getDoubleAttribute("pan", 0.0), 0.0f, -1.0f, 1.0f));
        strip->setPlaybackSpeed(1.0f);
        float speedRatio = 1.0f;
        if (stripXml->hasAttribute("speed"))
        {
            speedRatio = PlayheadSpeedQuantizer::quantizeRatio(
                static_cast<float>(stripXml->getDoubleAttribute("speed", 1.0)));
        }
        else if (stripXml->hasAttribute("beatsPerLoop"))
        {
            const int speedBars = clampedInt(stripXml->getIntAttribute("recordingBars", strip->getRecordingBars()),
                                             1, 8, 1);
            speedRatio = PlayheadSpeedQuantizer::quantizeRatio(
                PlayheadSpeedQuantizer::ratioFromBeatsPerLoop(
                    static_cast<float>(stripXml->getDoubleAttribute("beatsPerLoop", 4.0)),
                    speedBars));
        }
        strip->setPlayheadSpeedRatio(speedRatio);
        const int safeLoopStart = clampedInt(stripXml->getIntAttribute("loopStart", 0), 0, 15, 0);
        const int safeLoopEnd = clampedInt(stripXml->getIntAttribute("loopEnd", 16), 1, 16, 16);
        strip->setLoop(safeLoopStart, safeLoopEnd);
        strip->setPlayMode(static_cast<EnhancedAudioStrip::PlayMode>(
            clampedInt(stripXml->getIntAttribute("playMode", 1), 0, 4, 1)));
        strip->setDirectionMode(static_cast<EnhancedAudioStrip::DirectionMode>(
            clampedInt(stripXml->getIntAttribute("directionMode", 0), 0, 5, 0)));
        strip->setReverse(stripXml->getBoolAttribute("reversed", false));

        int groupId = stripXml->getIntAttribute("group", -1);
        audioEngine->assignStripToGroup(stripIndex, groupId);

        const bool restorePlayingRequested = stripXml->getBoolAttribute("isPlaying", false);
        const bool restorePlaying = canRecallPlayingState && restorePlayingRequested;
        const int restoreMarkerColumn = clampedInt(stripXml->getIntAttribute("playbackColumn", safeLoopStart),
                                                   0, ModernAudioEngine::MaxColumns - 1, safeLoopStart);
        const bool restorePpqAnchored = stripXml->getBoolAttribute("ppqTimelineAnchored", false);
        const double restorePpqOffsetBeats = stripXml->getDoubleAttribute("ppqTimelineOffsetBeats", 0.0);
        const int64_t restoreGlobalSample = audioEngine->getGlobalSampleCount();
        const double restoreTimelineBeat = recallPpq;
        const double restoreTempo = recallTempo;

        if (strip->hasAudio())
        {
            if (restorePlaying)
                audioEngine->enforceGroupExclusivity(stripIndex, false);

            strip->restorePresetPpqState(restorePlaying,
                                         restorePpqAnchored,
                                         restorePpqOffsetBeats,
                                         restoreMarkerColumn,
                                         restoreTempo,
                                         restoreTimelineBeat,
                                         restoreGlobalSample);
        }
        else
        {
            strip->stop(true);
        }

        float beats = finiteFloat(stripXml->getDoubleAttribute("beatsPerLoop", -1.0), -1.0f);
        strip->setBeatsPerLoop(beats);
        strip->setScratchAmount(clampedFloat(stripXml->getDoubleAttribute("scratchAmount", 0.0), 0.0f, 0.0f, 100.0f));
        const int analysisSampleCount = juce::jmax(0, stripXml->getIntAttribute(kAnalysisSampleCountAttr, 0));
        const juce::String analysisTransientCsv = stripXml->getStringAttribute(kAnalysisTransientAttr);
        const juce::String analysisRmsCsv = stripXml->getStringAttribute(kAnalysisRmsAttr);
        const juce::String analysisZeroCsv = stripXml->getStringAttribute(kAnalysisZeroCrossAttr);
        if (strip->hasAudio()
            && analysisSampleCount > 0
            && analysisTransientCsv.isNotEmpty()
            && analysisRmsCsv.isNotEmpty()
            && analysisZeroCsv.isNotEmpty())
        {
            std::array<int, 16> cachedTransient{};
            std::array<float, 128> cachedRms{};
            std::array<int, 128> cachedZeroCross{};
            decodeIntArrayCsv(analysisTransientCsv, cachedTransient);
            decodeFloatArrayCsv(analysisRmsCsv, cachedRms);
            decodeIntArrayCsv(analysisZeroCsv, cachedZeroCross);
            strip->restoreSampleAnalysisCache(cachedTransient, cachedRms, cachedZeroCross, analysisSampleCount);
        }
        strip->setTransientSliceMode(stripXml->getBoolAttribute("transientSliceMode", false));
        strip->setLoopSliceLength(clampedFloat(stripXml->getDoubleAttribute("loopSliceLength", 1.0), 1.0f, 0.0f, 1.0f));
        strip->setPitchShift(clampedFloat(stripXml->getDoubleAttribute("pitchShift", 0.0), 0.0f, -24.0f, 24.0f));
        strip->setRecordingBars(clampedInt(stripXml->getIntAttribute("recordingBars", 1), 1, 8, 1));
        const bool restoreFilterEnabled = stripXml->getBoolAttribute("filterEnabled", false);
        strip->setFilterFrequency(clampedFloat(stripXml->getDoubleAttribute("filterFrequency", 20000.0), 20000.0f, 20.0f, 20000.0f));
        strip->setFilterResonance(clampedFloat(stripXml->getDoubleAttribute("filterResonance", 0.707), 0.707f, 0.1f, 10.0f));
        if (stripXml->hasAttribute("filterMorph"))
            strip->setFilterMorph(clampedFloat(stripXml->getDoubleAttribute("filterMorph", 0.0), 0.0f, 0.0f, 1.0f));
        else
            strip->setFilterType(static_cast<EnhancedAudioStrip::FilterType>(
                clampedInt(stripXml->getIntAttribute("filterType", 0), 0, 2, 0)));

        strip->setFilterAlgorithm(static_cast<EnhancedAudioStrip::FilterAlgorithm>(
            clampedInt(stripXml->getIntAttribute("filterAlgorithm", 0), 0, 5, 0)));
        strip->setFilterEnabled(restoreFilterEnabled);
        strip->setSwingAmount(clampedFloat(stripXml->getDoubleAttribute("swingAmount", 0.0), 0.0f, 0.0f, 1.0f));
        strip->setGateAmount(clampedFloat(stripXml->getDoubleAttribute("gateAmount", 0.0), 0.0f, 0.0f, 1.0f));
        strip->setGateSpeed(clampedFloat(stripXml->getDoubleAttribute("gateSpeed", 4.0), 4.0f, 0.25f, 16.0f));
        strip->setGateEnvelope(clampedFloat(stripXml->getDoubleAttribute("gateEnvelope", 0.5), 0.5f, 0.0f, 1.0f));
        if (stripXml->hasAttribute("gateShapeCurve"))
        {
            strip->setGateShape(clampedFloat(stripXml->getDoubleAttribute("gateShapeCurve", 0.5), 0.5f, 0.0f, 1.0f));
        }
        else
        {
            // Backward compatibility for older enum presets:
            // Sine(0)->0.50, Triangle(1)->0.75, Square(2)->0.20
            const int legacyShape = clampedInt(stripXml->getIntAttribute("gateShape", 0), 0, 2, 0);
            float mappedShape = 0.5f;
            if (legacyShape == 1)
                mappedShape = 0.75f;
            else if (legacyShape == 2)
                mappedShape = 0.2f;
            strip->setGateShape(mappedShape);
        }

        const int stepPatternSteps = clampedInt(
            stripXml->getIntAttribute("stepPatternSteps",
                                      clampedInt(stripXml->getIntAttribute("stepPatternBars", 1), 1, 4, 1) * 16),
            1, 64, 16);
        strip->setStepPatternLengthSteps(stepPatternSteps);
        strip->setStepPage(clampedInt(stripXml->getIntAttribute("stepViewPage", 0), 0, 3, 0));
        strip->currentStep = juce::jmax(0, stripXml->getIntAttribute("stepCurrent", 0));
        decodeStepPatternBits(stripXml->getStringAttribute("stepPatternBits"), strip->stepPattern);
        decodeStepSubdivisions(stripXml->getStringAttribute("stepSubdivisions"), strip->stepSubdivisions);
        decodeStepSubdivisionRepeatVelocity(
            stripXml->getStringAttribute("stepSubdivisionStartVelocity"),
            strip->stepSubdivisionStartVelocity);
        decodeStepSubdivisionRepeatVelocity(
            stripXml->getStringAttribute("stepSubdivisionRepeatVelocity"),
            strip->stepSubdivisionRepeatVelocity);
        decodeStepSubdivisionRepeatVelocity(
            stripXml->getStringAttribute("stepProbability"),
            strip->stepProbability);
        strip->setStepPatternLengthSteps(stepPatternSteps);
        strip->setStepEnvelopeAttackMs(clampedFloat(stripXml->getDoubleAttribute("stepAttackMs", 0.0), 0.0f, 0.0f, 400.0f));
        strip->setStepEnvelopeDecayMs(clampedFloat(stripXml->getDoubleAttribute("stepDecayMs", 4000.0), 4000.0f, 1.0f, 4000.0f));
        strip->setStepEnvelopeReleaseMs(clampedFloat(stripXml->getDoubleAttribute("stepReleaseMs", 110.0), 110.0f, 1.0f, 4000.0f));

        strip->setGrainSizeMs(static_cast<float>(stripXml->getDoubleAttribute("grainSizeMs", 1240.0)));
        strip->setGrainDensity(static_cast<float>(stripXml->getDoubleAttribute("grainDensity", 0.05)));
        strip->setGrainPitch(clampedFloat(stripXml->getDoubleAttribute("grainPitch", 0.0), 0.0f, -48.0f, 48.0f));
        strip->setGrainPitchJitter(static_cast<float>(stripXml->getDoubleAttribute("grainPitchJitter", 0.0)));
        strip->setGrainSpread(static_cast<float>(stripXml->getDoubleAttribute("grainSpread", 0.0)));
        strip->setGrainJitter(static_cast<float>(stripXml->getDoubleAttribute("grainJitter", 0.0)));
        strip->setGrainPositionJitter(static_cast<float>(stripXml->getDoubleAttribute("grainPositionJitter", 0.0)));
        strip->setGrainRandomDepth(static_cast<float>(stripXml->getDoubleAttribute("grainRandomDepth", 0.0)));
        strip->setGrainArpDepth(static_cast<float>(stripXml->getDoubleAttribute("grainArpDepth", 0.0)));
        strip->setGrainCloudDepth(static_cast<float>(stripXml->getDoubleAttribute("grainCloudDepth", 0.0)));
        strip->setGrainEmitterDepth(static_cast<float>(stripXml->getDoubleAttribute("grainEmitterDepth", 0.0)));
        strip->setGrainEnvelope(static_cast<float>(stripXml->getDoubleAttribute("grainEnvelope", 0.0)));
        strip->setGrainShape(clampedFloat(stripXml->getDoubleAttribute("grainShape", 0.0), 0.0f, -1.0f, 1.0f));
        strip->setGrainArpMode(clampedInt(stripXml->getIntAttribute("grainArpMode", 0), 0, 5, 0));
        strip->setGrainTempoSyncEnabled(stripXml->getBoolAttribute("grainTempoSync", true));

        const int requestedActiveModSlot = clampedInt(
            stripXml->getIntAttribute("modActiveSequencer", 0),
            0,
            ModernAudioEngine::NumModSequencers - 1,
            0);

        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            audioEngine->setModSequencerSlot(stripIndex, modSlot);
            auto slotKey = [modSlot](const juce::String& suffix)
            {
                if (modSlot == 0)
                    return "mod" + suffix;
                return "mod" + juce::String(modSlot + 1) + suffix;
            };

            const auto targetKey = slotKey("Target");
            const auto stepsKey = slotKey("Steps");
            if (modSlot > 0 && !stripXml->hasAttribute(targetKey) && stripXml->getStringAttribute(stepsKey).isEmpty())
                continue;

            audioEngine->setModTarget(stripIndex,
                static_cast<ModernAudioEngine::ModTarget>(clampedInt(stripXml->getIntAttribute(targetKey, 0), 0, 18, 0)));
            audioEngine->setModBipolar(stripIndex, stripXml->getBoolAttribute(slotKey("Bipolar"), false));
            audioEngine->setModCurveMode(stripIndex, stripXml->getBoolAttribute(slotKey("CurveMode"), false));
            audioEngine->setModDepth(stripIndex, clampedFloat(stripXml->getDoubleAttribute(slotKey("Depth"), 1.0), 1.0f, 0.0f, 1.0f));
            audioEngine->setModOffset(stripIndex, clampedInt(stripXml->getIntAttribute(slotKey("Offset"), 0), -127, 127, 0));
            audioEngine->setModLengthBars(stripIndex, clampedInt(stripXml->getIntAttribute(slotKey("LengthBars"), 1), 1, 8, 1));
            audioEngine->setModEditPage(stripIndex, clampedInt(stripXml->getIntAttribute(slotKey("EditPage"), 0), 0, 7, 0));
            audioEngine->setModSmoothingMs(stripIndex, clampedFloat(stripXml->getDoubleAttribute(slotKey("SmoothMs"), 0.0), 0.0f, 0.0f, 250.0f));
            audioEngine->setModCurveBend(stripIndex, clampedFloat(stripXml->getDoubleAttribute(slotKey("CurveBend"), 0.0), 0.0f, -1.0f, 1.0f));
            const int modCurveShapeIndex = clampedInt(stripXml->getIntAttribute(slotKey("CurveShape"), 0), 0, 4, 0);
            audioEngine->setModCurveShape(stripIndex, static_cast<ModernAudioEngine::ModCurveShape>(modCurveShapeIndex));
            audioEngine->setModPitchScaleQuantize(stripIndex, stripXml->getBoolAttribute(slotKey("PitchScaleQuantize"), false));
            audioEngine->setModPitchScale(stripIndex, static_cast<ModernAudioEngine::PitchScale>(
                clampedInt(stripXml->getIntAttribute(slotKey("PitchScale"), 0), 0, 4, 0)));
            std::array<float, ModernAudioEngine::ModTotalSteps> modSteps{};
            decodeModSteps(stripXml->getStringAttribute(stepsKey), modSteps);
            std::array<int, ModernAudioEngine::ModTotalSteps> modStepSubdivisions{};
            modStepSubdivisions.fill(1);
            std::array<float, ModernAudioEngine::ModTotalSteps> modStepEndValues = modSteps;
            std::array<int, ModernAudioEngine::ModTotalSteps> modStepCurveShapes{};
            modStepCurveShapes.fill(modCurveShapeIndex);
            const auto modSubdivText = stripXml->getStringAttribute(slotKey("StepSubdivisions"));
            const auto modEndText = stripXml->getStringAttribute(slotKey("StepEndValues"));
            const auto modCurvePerStepText = stripXml->getStringAttribute(slotKey("StepCurveShapes"));
            const bool hasModShapeData = modSubdivText.isNotEmpty() || modEndText.isNotEmpty();
            const bool hasModCurvePerStepData = modCurvePerStepText.isNotEmpty();
            if (hasModShapeData)
            {
                if (modSubdivText.isNotEmpty())
                    decodeIntArrayCsv(modSubdivText, modStepSubdivisions);
                if (modEndText.isNotEmpty())
                    decodeFloatArrayCsv(modEndText, modStepEndValues);
            }
            if (hasModCurvePerStepData)
                decodeIntArrayCsv(modCurvePerStepText, modStepCurveShapes);
            for (int s = 0; s < ModernAudioEngine::ModTotalSteps; ++s)
            {
                const float startValue = juce::jlimit(0.0f, 1.0f, modSteps[static_cast<size_t>(s)]);
                audioEngine->setModStepValueAbsolute(stripIndex, s, startValue);
                if (hasModShapeData)
                {
                    const int subdivisions = juce::jlimit(
                        1, ModernAudioEngine::ModMaxStepSubdivisions, modStepSubdivisions[static_cast<size_t>(s)]);
                    const float endValue = juce::jlimit(0.0f, 1.0f, modStepEndValues[static_cast<size_t>(s)]);
                    audioEngine->setModStepShapeAbsolute(stripIndex, s, subdivisions, endValue);
                }
                const int stepCurveShapeIndex = juce::jlimit(
                    0,
                    static_cast<int>(ModernAudioEngine::ModCurveShape::Square),
                    modStepCurveShapes[static_cast<size_t>(s)]);
                audioEngine->setModStepCurveShapeAbsolute(
                    stripIndex,
                    s,
                    static_cast<ModernAudioEngine::ModCurveShape>(stepCurveShapeIndex));
            }
        }

        audioEngine->setModSequencerSlot(stripIndex, requestedActiveModSlot);

        if (auto* volParam = parameters.getParameter("stripVolume" + juce::String(stripIndex)))
            volParam->setValueNotifyingHost(static_cast<float>(stripXml->getDoubleAttribute("volume", 1.0)));

        if (auto* panParam = parameters.getParameter("stripPan" + juce::String(stripIndex)))
        {
            float panValue = static_cast<float>(stripXml->getDoubleAttribute("pan", 0.0));
            panParam->setValueNotifyingHost((panValue + 1.0f) * 0.5f);
        }

        if (auto* speedParam = parameters.getParameter("stripSpeed" + juce::String(stripIndex)))
        {
            const float speedValue = PlayheadSpeedQuantizer::quantizeRatio(strip->getPlayheadSpeedRatio());
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(speedParam))
            {
                speedParam->setValueNotifyingHost(
                    juce::jlimit(0.0f, 1.0f, ranged->convertTo0to1(speedValue)));
            }
        }

        if (auto* pitchParam = parameters.getParameter("stripPitch" + juce::String(stripIndex)))
        {
            float pitchValue = static_cast<float>(stripXml->getDoubleAttribute("pitchShift", 0.0));
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(pitchParam))
            {
                pitchParam->setValueNotifyingHost(
                    juce::jlimit(0.0f, 1.0f, ranged->convertTo0to1(pitchValue)));
                }
        }

        if (auto* sliceLengthParam = parameters.getParameter("stripSliceLength" + juce::String(stripIndex)))
        {
            float sliceLengthValue = static_cast<float>(stripXml->getDoubleAttribute("loopSliceLength", 1.0));
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(sliceLengthParam))
            {
                sliceLengthParam->setValueNotifyingHost(
                    juce::jlimit(0.0f, 1.0f, ranged->convertTo0to1(sliceLengthValue)));
            }
        }
    }

    for (int i = 0; i < safeMaxStrips; ++i)
    {
        if (stripSeen[static_cast<size_t>(i)])
            continue;

        if (auto* strip = audioEngine->getStrip(i))
            resetStripToDefaultState(i, *strip, *audioEngine, parameters);
        else
            resetStripParametersToDefaults(parameters, i);
    }

    for (int i = 0; i < ModernAudioEngine::MaxGroups; ++i)
    {
        if (auto* group = audioEngine->getGroup(i))
        {
            group->setVolume(1.0f);
            group->setMuted(false);
        }
    }

    if (auto* groupsXml = preset->getChildByName("Groups"))
    {
        for (auto* groupXml : groupsXml->getChildIterator())
        {
            if (groupXml->getTagName() != "Group")
                continue;
            const int index = groupXml->getIntAttribute("index", -1);
            if (auto* group = audioEngine->getGroup(index))
            {
                group->setVolume(static_cast<float>(groupXml->getDoubleAttribute("volume", 1.0)));
                group->setMuted(groupXml->getBoolAttribute("muted", false));
            }
        }
    }

    for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
        audioEngine->clearPattern(i);

    if (auto* patternsXml = preset->getChildByName("Patterns"))
    {
        const double nowBeat = audioEngine->getTimelineBeat();
        for (auto* patternXml : patternsXml->getChildIterator())
        {
            if (patternXml->getTagName() != "Pattern")
                continue;
            const int index = patternXml->getIntAttribute("index", -1);
            auto* pattern = audioEngine->getPattern(index);
            if (!pattern)
                continue;

            std::vector<PatternRecorder::Event> events;
            for (auto* eventXml : patternXml->getChildIterator())
            {
                if (eventXml->getTagName() != "Event")
                    continue;
                PatternRecorder::Event e{};
                e.stripIndex = eventXml->getIntAttribute("strip", 0);
                e.column = eventXml->getIntAttribute("column", 0);
                e.time = eventXml->getDoubleAttribute("time", 0.0);
                e.isNoteOn = eventXml->getBoolAttribute("noteOn", true);
                events.push_back(e);
            }

            const int lengthBeats = patternXml->getIntAttribute("lengthBeats", 4);
            pattern->setEventsSnapshot(events, lengthBeats);
            if (canRecallPlayingState && patternXml->getBoolAttribute("isPlaying", false) && !events.empty())
                pattern->startPlayback(nowBeat);
        }
    }

        DBG("Preset " << (presetIndex + 1) << " loaded");
        return true;
    }
    catch (const std::exception& e)
    {
        DBG("Preset load failed for slot " << (presetIndex + 1) << ": " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("Preset load failed for slot " << (presetIndex + 1) << ": unknown exception");
        return false;
    }

    return false;
}

juce::String getPresetName(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return {};
    auto presetDir = getPresetDirectory();
    auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
    if (presetFile.existsAsFile() && isPresetFileSizeValid(presetFile, kMaxPresetNameXmlBytes))
    {
        if (auto preset = parsePresetXmlSafely(presetFile, kMaxPresetNameXmlBytes))
        {
            const auto storedName = preset->getStringAttribute("name").trim();
            if (storedName.isNotEmpty())
                return storedName;
        }
    }
    return "Preset " + juce::String(presetIndex + 1);
}

bool setPresetName(int presetIndex, const juce::String& presetName)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return false;

    try
    {
        auto presetDir = getPresetDirectory();
        auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
        if (!presetFile.existsAsFile())
        {
            if (!writeDefaultPresetFile(presetFile, presetIndex))
                return false;
        }

        auto preset = parsePresetXmlSafely(presetFile, kMaxPresetNameXmlBytes);
        if (!preset)
            return false;

        const auto trimmed = presetName.trim();
        if (trimmed.isNotEmpty())
            preset->setAttribute("name", trimmed);
        else
            preset->removeAttribute("name");

        return writePresetAtomically(*preset, presetFile);
    }
    catch (...)
    {
        return false;
    }
}

bool presetExists(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return false;

    auto presetDir = getPresetDirectory();
    auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
    return presetFile.existsAsFile();
}

bool deletePreset(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return false;

    try
    {
        auto presetDir = getPresetDirectory();
        auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
        if (!presetFile.existsAsFile())
            return false;

        return presetFile.deleteFile();
    }
    catch (...)
    {
        return false;
    }
}
} // namespace PresetStore
