#include "PresetStore.h"
#include "AudioEngine.h"

namespace PresetStore
{
static constexpr int kMaxPresetSlots = 16 * 7;

namespace
{
struct GlobalParameterSnapshot
{
    float masterVolume = 1.0f;
    float quantizeChoice = 5.0f;
    float grainQuality = 2.0f;
    float pitchSmoothing = 0.05f;
    float inputMonitor = 1.0f;
    float crossfadeMs = 10.0f;
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

bool writeDefaultPresetFile(const juce::File& presetFile, int presetIndex)
{
    juce::XmlElement preset("mlrVSTPreset");
    preset.setAttribute("version", "1.0");
    preset.setAttribute("index", presetIndex);

    auto* globalsXml = preset.createNewChildElement("Globals");
    globalsXml->setAttribute("masterVolume", 0.7);
    globalsXml->setAttribute("quantize", 5);
    globalsXml->setAttribute("crossfadeLength", 10.0);

    return preset.writeTo(presetFile);
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

    auto presetDir = getPresetDirectory();
    if (!presetDir.exists())
        presetDir.createDirectory();

    auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");

    juce::XmlElement preset("mlrVSTPreset");
    preset.setAttribute("version", "1.0");
    preset.setAttribute("index", presetIndex);

    for (int i = 0; i < maxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (strip == nullptr)
            continue;

        auto* stripXml = preset.createNewChildElement("Strip");
        stripXml->setAttribute("index", i);

        if (strip->hasAudio() && currentStripFiles[i] != juce::File())
            stripXml->setAttribute("samplePath", currentStripFiles[i].getFullPathName());

        stripXml->setAttribute("volume", strip->getVolume());
        stripXml->setAttribute("pan", strip->getPan());
        stripXml->setAttribute("speed", strip->getPlaybackSpeed());
        stripXml->setAttribute("loopStart", strip->getLoopStart());
        stripXml->setAttribute("loopEnd", strip->getLoopEnd());
        stripXml->setAttribute("playMode", static_cast<int>(strip->getPlayMode()));
        stripXml->setAttribute("directionMode", static_cast<int>(strip->getDirectionMode()));
        stripXml->setAttribute("reversed", strip->isReversed());
        stripXml->setAttribute("group", strip->getGroup());
        stripXml->setAttribute("beatsPerLoop", strip->getBeatsPerLoop());
        stripXml->setAttribute("scratchAmount", strip->getScratchAmount());
        stripXml->setAttribute("transientSliceMode", strip->isTransientSliceMode());
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

void loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<void(int, const juce::File&)>& loadSampleToStrip)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots || audioEngine == nullptr)
        return;

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

    for (auto* stripXml : preset->getChildWithTagNameIterator("Strip"))
    {
        int stripIndex = stripXml->getIntAttribute("index");
        if (stripIndex < 0 || stripIndex >= maxStrips)
            continue;

        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        juce::String samplePath = stripXml->getStringAttribute("samplePath");
        if (samplePath.isNotEmpty())
        {
            juce::File sampleFile(samplePath);
            if (sampleFile.existsAsFile())
                loadSampleToStrip(stripIndex, sampleFile);
        }

        strip->setVolume(static_cast<float>(stripXml->getDoubleAttribute("volume", 1.0)));
        strip->setPan(static_cast<float>(stripXml->getDoubleAttribute("pan", 0.0)));
        strip->setPlaybackSpeed(static_cast<float>(stripXml->getDoubleAttribute("speed", 1.0)));
        strip->setLoop(stripXml->getIntAttribute("loopStart", 0),
                       stripXml->getIntAttribute("loopEnd", 16));
        strip->setPlayMode(static_cast<EnhancedAudioStrip::PlayMode>(
            stripXml->getIntAttribute("playMode", 1)));
        strip->setDirectionMode(static_cast<EnhancedAudioStrip::DirectionMode>(
            stripXml->getIntAttribute("directionMode", 0)));
        strip->setReverse(stripXml->getBoolAttribute("reversed", false));

        int groupId = stripXml->getIntAttribute("group", -1);
        audioEngine->assignStripToGroup(stripIndex, groupId);

        float beats = static_cast<float>(stripXml->getDoubleAttribute("beatsPerLoop", -1.0));
        strip->setBeatsPerLoop(beats);
        strip->setScratchAmount(static_cast<float>(stripXml->getDoubleAttribute("scratchAmount", 0.0)));
        strip->setTransientSliceMode(stripXml->getBoolAttribute("transientSliceMode", false));
        strip->setPitchShift(static_cast<float>(stripXml->getDoubleAttribute("pitchShift", 0.0)));
        strip->setRecordingBars(stripXml->getIntAttribute("recordingBars", 1));
        strip->setFilterEnabled(stripXml->getBoolAttribute("filterEnabled", false));
        strip->setFilterFrequency(static_cast<float>(stripXml->getDoubleAttribute("filterFrequency", 20000.0)));
        strip->setFilterResonance(static_cast<float>(stripXml->getDoubleAttribute("filterResonance", 0.707)));
        strip->setFilterType(static_cast<EnhancedAudioStrip::FilterType>(
            stripXml->getIntAttribute("filterType", 0)));
        strip->setSwingAmount(static_cast<float>(stripXml->getDoubleAttribute("swingAmount", 0.0)));
        strip->setGateAmount(static_cast<float>(stripXml->getDoubleAttribute("gateAmount", 0.0)));
        strip->setGateSpeed(static_cast<float>(stripXml->getDoubleAttribute("gateSpeed", 4.0)));
        strip->setGateEnvelope(static_cast<float>(stripXml->getDoubleAttribute("gateEnvelope", 0.5)));
        strip->setGateShape(static_cast<EnhancedAudioStrip::GateShape>(
            stripXml->getIntAttribute("gateShape", 0)));

        strip->setStepPatternBars(stripXml->getIntAttribute("stepPatternBars", 1));
        strip->setStepPage(stripXml->getIntAttribute("stepViewPage", 0));
        strip->currentStep = juce::jmax(0, stripXml->getIntAttribute("stepCurrent", 0));
        decodeStepPatternBits(stripXml->getStringAttribute("stepPatternBits"), strip->stepPattern);

        strip->setGrainSizeMs(static_cast<float>(stripXml->getDoubleAttribute("grainSizeMs", strip->getGrainSizeMs())));
        strip->setGrainDensity(static_cast<float>(stripXml->getDoubleAttribute("grainDensity", strip->getGrainDensity())));
        strip->setGrainPitch(static_cast<float>(stripXml->getDoubleAttribute("grainPitch", strip->getGrainPitch())));
        strip->setGrainPitchJitter(static_cast<float>(stripXml->getDoubleAttribute("grainPitchJitter", strip->getGrainPitchJitter())));
        strip->setGrainSpread(static_cast<float>(stripXml->getDoubleAttribute("grainSpread", strip->getGrainSpread())));
        strip->setGrainJitter(static_cast<float>(stripXml->getDoubleAttribute("grainJitter", strip->getGrainJitter())));
        strip->setGrainRandomDepth(static_cast<float>(stripXml->getDoubleAttribute("grainRandomDepth", strip->getGrainRandomDepth())));
        strip->setGrainArpDepth(static_cast<float>(stripXml->getDoubleAttribute("grainArpDepth", strip->getGrainArpDepth())));
        strip->setGrainCloudDepth(static_cast<float>(stripXml->getDoubleAttribute("grainCloudDepth", strip->getGrainCloudDepth())));
        strip->setGrainEmitterDepth(static_cast<float>(stripXml->getDoubleAttribute("grainEmitterDepth", strip->getGrainEmitterDepth())));
        strip->setGrainEnvelope(static_cast<float>(stripXml->getDoubleAttribute("grainEnvelope", strip->getGrainEnvelope())));
        strip->setGrainArpMode(stripXml->getIntAttribute("grainArpMode", strip->getGrainArpMode()));
        strip->setGrainTempoSyncEnabled(stripXml->getBoolAttribute("grainTempoSync", strip->isGrainTempoSyncEnabled()));

        audioEngine->setModTarget(stripIndex,
            static_cast<ModernAudioEngine::ModTarget>(stripXml->getIntAttribute("modTarget", 0)));
        audioEngine->setModBipolar(stripIndex, stripXml->getBoolAttribute("modBipolar", false));
        audioEngine->setModCurveMode(stripIndex, stripXml->getBoolAttribute("modCurveMode", false));
        audioEngine->setModDepth(stripIndex, static_cast<float>(stripXml->getDoubleAttribute("modDepth", 1.0)));
        audioEngine->setModOffset(stripIndex, stripXml->getIntAttribute("modOffset", 0));
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
            auto speedRange = juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f, 0.5f);
            speedParam->setValueNotifyingHost(speedRange.convertTo0to1(speedValue));
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

    if (auto* patternsXml = preset->getChildByName("Patterns"))
    {
        const double nowBeat = audioEngine->getTimelineBeat();
        for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
            audioEngine->clearPattern(i);

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

juce::String getPresetName(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= kMaxPresetSlots)
        return {};

    return "Preset " + juce::String(presetIndex + 1);
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

    auto presetDir = getPresetDirectory();
    auto presetFile = presetDir.getChildFile("Preset_" + juce::String(presetIndex + 1) + ".mlrpreset");
    if (!presetFile.existsAsFile())
        return false;

    return presetFile.deleteFile();
}
} // namespace PresetStore
