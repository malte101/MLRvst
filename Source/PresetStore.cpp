#include "PresetStore.h"
#include "AudioEngine.h"
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

struct GlobalParameterSnapshot
{
    float masterVolume = 1.0f;
    float quantizeChoice = 5.0f;
    float grainQuality = 2.0f;
    float pitchSmoothing = 0.05f;
    float inputMonitor = 1.0f;
    float crossfadeMs = 10.0f;
    float triggerFadeInMs = 12.0f;
};

GlobalParameterSnapshot captureGlobalParameters(juce::AudioProcessorValueTreeState& parameters)
{
    GlobalParameterSnapshot snapshot;
    if (auto* p = parameters.getRawParameterValue("masterVolume"))
        snapshot.masterVolume = *p;
    if (auto* p = parameters.getRawParameterValue("quantize"))
        snapshot.quantizeChoice = *p;
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
    return snapshot;
}

void restoreGlobalParameters(juce::AudioProcessorValueTreeState& parameters, const GlobalParameterSnapshot& snapshot)
{
    if (auto* param = parameters.getParameter("masterVolume"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.masterVolume));
    if (auto* param = parameters.getParameter("quantize"))
        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, snapshot.quantizeChoice / 9.0f));
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

juce::String encodeModSteps(const std::array<float, ModernAudioEngine::ModSteps>& steps)
{
    juce::String out;
    out.preallocateBytes(ModernAudioEngine::ModSteps);
    for (float v : steps)
        out += (v >= 0.5f ? "1" : "0");
    return out;
}

void decodeModSteps(const juce::String& text, std::array<float, ModernAudioEngine::ModSteps>& steps)
{
    for (auto& v : steps)
        v = 0.0f;
    const int len = juce::jmin(ModernAudioEngine::ModSteps, text.length());
    for (int i = 0; i < len; ++i)
        steps[static_cast<size_t>(i)] = (text[i] == '1') ? 1.0f : 0.0f;
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
        if (auto existing = juce::XmlDocument::parse(presetFile))
        {
            const auto existingName = existing->getStringAttribute("name").trim();
            if (existingName.isNotEmpty())
                preset.setAttribute("name", existingName);
        }
    }

    auto* globalsXml = preset.createNewChildElement("Globals");
    globalsXml->setAttribute("masterVolume", 0.7);
    globalsXml->setAttribute("quantize", 5);
    globalsXml->setAttribute("crossfadeLength", 10.0);

    return preset.writeTo(presetFile);
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
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(wavBytesRaw,
                                  sampleRate,
                                  static_cast<unsigned int>(buffer.getNumChannels()),
                                  24,
                                  {},
                                  0));

    if (!writer)
        return false;

    // createWriterFor transfers stream ownership to the writer on success.
    wavBytes.release();

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

void savePreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const juce::File* currentStripFiles)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots || audioEngine == nullptr || currentStripFiles == nullptr)
        return;

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
            if (auto existing = juce::XmlDocument::parse(presetFile))
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
            if (currentStripFiles[i] != juce::File())
            {
                stripXml->setAttribute("samplePath", currentStripFiles[i].getFullPathName());
            }
            else if (const auto* audioBuffer = strip->getAudioBuffer())
            {
                juce::String embeddedWav;
                if (encodeBufferAsWavBase64(*audioBuffer, strip->getSourceSampleRate(), embeddedWav))
                    stripXml->setAttribute(kEmbeddedSampleAttr, embeddedWav);
            }
        }

        stripXml->setAttribute("volume", strip->getVolume());
        stripXml->setAttribute("pan", strip->getPan());
        stripXml->setAttribute("speed", strip->getPlaybackSpeed());
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
        stripXml->setAttribute("filterType", static_cast<int>(strip->getFilterType()));
        stripXml->setAttribute("swingAmount", strip->getSwingAmount());
        stripXml->setAttribute("gateAmount", strip->getGateAmount());
        stripXml->setAttribute("gateSpeed", strip->getGateSpeed());
        stripXml->setAttribute("gateEnvelope", strip->getGateEnvelope());
        stripXml->setAttribute("gateShape", static_cast<int>(strip->getGateShape()));
        stripXml->setAttribute("stepPatternBars", strip->getStepPatternBars());
        stripXml->setAttribute("stepViewPage", strip->getStepPage());
        stripXml->setAttribute("stepCurrent", strip->currentStep);
        stripXml->setAttribute("stepPatternBits", encodeStepPatternBits(strip->stepPattern));

        stripXml->setAttribute("grainSizeMs", strip->getGrainSizeMs());
        stripXml->setAttribute("grainDensity", strip->getGrainDensity());
        stripXml->setAttribute("grainPitch", strip->getGrainPitch());
        stripXml->setAttribute("grainPitchJitter", strip->getGrainPitchJitter());
        stripXml->setAttribute("grainSpread", strip->getGrainSpread());
        stripXml->setAttribute("grainJitter", strip->getGrainJitter());
        stripXml->setAttribute("grainRandomDepth", strip->getGrainRandomDepth());
        stripXml->setAttribute("grainArpDepth", strip->getGrainArpDepth());
        stripXml->setAttribute("grainCloudDepth", strip->getGrainCloudDepth());
        stripXml->setAttribute("grainEmitterDepth", strip->getGrainEmitterDepth());
        stripXml->setAttribute("grainEnvelope", strip->getGrainEnvelope());
        stripXml->setAttribute("grainArpMode", strip->getGrainArpMode());
        stripXml->setAttribute("grainTempoSync", strip->isGrainTempoSyncEnabled());

        const auto mod = audioEngine->getModSequencerState(i);
        stripXml->setAttribute("modTarget", static_cast<int>(mod.target));
        stripXml->setAttribute("modBipolar", mod.bipolar);
        stripXml->setAttribute("modCurveMode", mod.curveMode);
        stripXml->setAttribute("modDepth", mod.depth);
        stripXml->setAttribute("modOffset", mod.offset);
        stripXml->setAttribute("modSteps", encodeModSteps(mod.steps));
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
    if (auto* crossfade = parameters.getRawParameterValue("crossfadeLength"))
        globalsXml->setAttribute("crossfadeLength", *crossfade);

        if (preset.writeTo(presetFile))
            DBG("Preset " << (presetIndex + 1) << " saved: " << presetFile.getFullPathName());
    }
    catch (const std::exception& e)
    {
        DBG("Preset save failed for slot " << (presetIndex + 1) << ": " << e.what());
    }
    catch (...)
    {
        DBG("Preset save failed for slot " << (presetIndex + 1) << ": unknown exception");
    }
}

void loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<void(int, const juce::File&)>& loadSampleToStrip,
                double hostPpqSnapshot,
                double hostTempoSnapshot)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots || audioEngine == nullptr)
        return;

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
            return;
        }
    }

    auto preset = juce::XmlDocument::parse(presetFile);
    if (!preset || preset->getTagName() != "mlrVSTPreset")
    {
        // Attempt self-heal for corrupt files.
        if (!writeDefaultPresetFile(presetFile, presetIndex))
        {
            DBG("Invalid preset file and recovery failed");
            return;
        }
        preset = juce::XmlDocument::parse(presetFile);
        if (!preset || preset->getTagName() != "mlrVSTPreset")
        {
            DBG("Invalid preset file after recovery");
            return;
        }
    }

        const auto globalSnapshot = captureGlobalParameters(parameters);

        if (auto* paramsXml = preset->getChildByName("ParametersState"))
        {
            auto state = juce::ValueTree::fromXml(*paramsXml);
            if (state.isValid())
                parameters.replaceState(state);
        }

        // Preset recall should not overwrite global controls.
        restoreGlobalParameters(parameters, globalSnapshot);

        const double recallPpq = std::isfinite(hostPpqSnapshot)
            ? hostPpqSnapshot
            : audioEngine->getTimelineBeat();
        const double recallTempo = (std::isfinite(hostTempoSnapshot) && hostTempoSnapshot > 0.0)
            ? hostTempoSnapshot
            : audioEngine->getCurrentTempo();

        std::vector<bool> stripSeen(static_cast<size_t>(juce::jmax(0, maxStrips)), false);
        for (auto* stripXml : preset->getChildWithTagNameIterator("Strip"))
        {
        int stripIndex = stripXml->getIntAttribute("index");
        if (stripIndex < 0 || stripIndex >= maxStrips)
            continue;

        stripSeen[static_cast<size_t>(stripIndex)] = true;
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const juce::String samplePath = stripXml->getStringAttribute("samplePath");
        bool loadedStripAudio = false;
        if (samplePath.isNotEmpty())
        {
            juce::File sampleFile(samplePath);
            if (sampleFile.existsAsFile())
            {
                loadSampleToStrip(stripIndex, sampleFile);
                loadedStripAudio = true;
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
        strip->setPlaybackSpeed(clampedFloat(stripXml->getDoubleAttribute("speed", 1.0), 1.0f, 0.0f, 4.0f));
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

        const bool restorePlaying = stripXml->getBoolAttribute("isPlaying", false);
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
        strip->setPitchShift(clampedFloat(stripXml->getDoubleAttribute("pitchShift", 0.0), 0.0f, -12.0f, 12.0f));
        strip->setRecordingBars(clampedInt(stripXml->getIntAttribute("recordingBars", 1), 1, 8, 1));
        strip->setFilterEnabled(stripXml->getBoolAttribute("filterEnabled", false));
        strip->setFilterFrequency(clampedFloat(stripXml->getDoubleAttribute("filterFrequency", 20000.0), 20000.0f, 20.0f, 20000.0f));
        strip->setFilterResonance(clampedFloat(stripXml->getDoubleAttribute("filterResonance", 0.707), 0.707f, 0.1f, 10.0f));
        strip->setFilterType(static_cast<EnhancedAudioStrip::FilterType>(
            clampedInt(stripXml->getIntAttribute("filterType", 0), 0, 2, 0)));
        strip->setSwingAmount(clampedFloat(stripXml->getDoubleAttribute("swingAmount", 0.0), 0.0f, 0.0f, 1.0f));
        strip->setGateAmount(clampedFloat(stripXml->getDoubleAttribute("gateAmount", 0.0), 0.0f, 0.0f, 1.0f));
        strip->setGateSpeed(clampedFloat(stripXml->getDoubleAttribute("gateSpeed", 4.0), 4.0f, 0.25f, 16.0f));
        strip->setGateEnvelope(clampedFloat(stripXml->getDoubleAttribute("gateEnvelope", 0.5), 0.5f, 0.0f, 1.0f));
        strip->setGateShape(static_cast<EnhancedAudioStrip::GateShape>(
            clampedInt(stripXml->getIntAttribute("gateShape", 0), 0, 2, 0)));

        strip->setStepPatternBars(clampedInt(stripXml->getIntAttribute("stepPatternBars", 1), 1, 4, 1));
        strip->setStepPage(clampedInt(stripXml->getIntAttribute("stepViewPage", 0), 0, 3, 0));
        strip->currentStep = juce::jmax(0, stripXml->getIntAttribute("stepCurrent", 0));
        decodeStepPatternBits(stripXml->getStringAttribute("stepPatternBits"), strip->stepPattern);

        strip->setGrainSizeMs(static_cast<float>(stripXml->getDoubleAttribute("grainSizeMs", strip->getGrainSizeMs())));
        strip->setGrainDensity(static_cast<float>(stripXml->getDoubleAttribute("grainDensity", strip->getGrainDensity())));
        strip->setGrainPitch(clampedFloat(stripXml->getDoubleAttribute("grainPitch", strip->getGrainPitch()), strip->getGrainPitch(), -48.0f, 48.0f));
        strip->setGrainPitchJitter(static_cast<float>(stripXml->getDoubleAttribute("grainPitchJitter", strip->getGrainPitchJitter())));
        strip->setGrainSpread(static_cast<float>(stripXml->getDoubleAttribute("grainSpread", strip->getGrainSpread())));
        strip->setGrainJitter(static_cast<float>(stripXml->getDoubleAttribute("grainJitter", strip->getGrainJitter())));
        strip->setGrainRandomDepth(static_cast<float>(stripXml->getDoubleAttribute("grainRandomDepth", strip->getGrainRandomDepth())));
        strip->setGrainArpDepth(static_cast<float>(stripXml->getDoubleAttribute("grainArpDepth", strip->getGrainArpDepth())));
        strip->setGrainCloudDepth(static_cast<float>(stripXml->getDoubleAttribute("grainCloudDepth", strip->getGrainCloudDepth())));
        strip->setGrainEmitterDepth(static_cast<float>(stripXml->getDoubleAttribute("grainEmitterDepth", strip->getGrainEmitterDepth())));
        strip->setGrainEnvelope(static_cast<float>(stripXml->getDoubleAttribute("grainEnvelope", strip->getGrainEnvelope())));
        strip->setGrainArpMode(clampedInt(stripXml->getIntAttribute("grainArpMode", strip->getGrainArpMode()), 0, 5, strip->getGrainArpMode()));
        strip->setGrainTempoSyncEnabled(stripXml->getBoolAttribute("grainTempoSync", strip->isGrainTempoSyncEnabled()));

        audioEngine->setModTarget(stripIndex,
            static_cast<ModernAudioEngine::ModTarget>(clampedInt(stripXml->getIntAttribute("modTarget", 0), 0, 17, 0)));
        audioEngine->setModBipolar(stripIndex, stripXml->getBoolAttribute("modBipolar", false));
        audioEngine->setModCurveMode(stripIndex, stripXml->getBoolAttribute("modCurveMode", false));
        audioEngine->setModDepth(stripIndex, clampedFloat(stripXml->getDoubleAttribute("modDepth", 1.0), 1.0f, 0.0f, 1.0f));
        audioEngine->setModOffset(stripIndex, clampedInt(stripXml->getIntAttribute("modOffset", 0), -15, 15, 0));
        std::array<float, ModernAudioEngine::ModSteps> modSteps{};
        decodeModSteps(stripXml->getStringAttribute("modSteps"), modSteps);
        for (int s = 0; s < ModernAudioEngine::ModSteps; ++s)
            audioEngine->setModStepValue(stripIndex, s, modSteps[static_cast<size_t>(s)]);

        if (auto* volParam = parameters.getParameter("stripVolume" + juce::String(stripIndex)))
            volParam->setValueNotifyingHost(static_cast<float>(stripXml->getDoubleAttribute("volume", 1.0)));

        if (auto* panParam = parameters.getParameter("stripPan" + juce::String(stripIndex)))
        {
            float panValue = static_cast<float>(stripXml->getDoubleAttribute("pan", 0.0));
            panParam->setValueNotifyingHost((panValue + 1.0f) * 0.5f);
        }

        if (auto* speedParam = parameters.getParameter("stripSpeed" + juce::String(stripIndex)))
        {
            float speedValue = static_cast<float>(stripXml->getDoubleAttribute("speed", 1.0));
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
    }

    for (int i = 0; i < maxStrips; ++i)
    {
        if (stripSeen[static_cast<size_t>(i)])
            continue;

        if (auto* strip = audioEngine->getStrip(i))
        {
            strip->clearSample();
            strip->stop(true);
            audioEngine->assignStripToGroup(i, -1);
        }
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
            if (patternXml->getBoolAttribute("isPlaying", false) && !events.empty())
                pattern->startPlayback(nowBeat);
        }
    }

        DBG("Preset " << (presetIndex + 1) << " loaded");
    }
    catch (const std::exception& e)
    {
        DBG("Preset load failed for slot " << (presetIndex + 1) << ": " << e.what());
    }
    catch (...)
    {
        DBG("Preset load failed for slot " << (presetIndex + 1) << ": unknown exception");
    }
}

juce::String getPresetName(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return {};
    auto presetDir = getPresetDirectory();
    auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
    if (presetFile.existsAsFile())
    {
        if (auto preset = juce::XmlDocument::parse(presetFile))
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

        auto preset = juce::XmlDocument::parse(presetFile);
        if (!preset || preset->getTagName() != "mlrVSTPreset")
            return false;

        const auto trimmed = presetName.trim();
        if (trimmed.isNotEmpty())
            preset->setAttribute("name", trimmed);
        else
            preset->removeAttribute("name");

        return preset->writeTo(presetFile);
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
