#include "PresetStore.h"
#include "AudioEngine.h"

namespace PresetStore
{
static constexpr int kMaxPresetSlots = 16 * 7;

namespace
{
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
        stripXml->setAttribute("reversed", strip->isReversed());
        stripXml->setAttribute("group", strip->getGroup());
        stripXml->setAttribute("beatsPerLoop", strip->getBeatsPerLoop());
        stripXml->setAttribute("scratchAmount", strip->getScratchAmount());
        stripXml->setAttribute("transientSliceMode", strip->isTransientSliceMode());
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
        strip->setReverse(stripXml->getBoolAttribute("reversed", false));

        int groupId = stripXml->getIntAttribute("group", -1);
        audioEngine->assignStripToGroup(stripIndex, groupId);

        float beats = static_cast<float>(stripXml->getDoubleAttribute("beatsPerLoop", -1.0));
        strip->setBeatsPerLoop(beats);
        strip->setScratchAmount(static_cast<float>(stripXml->getDoubleAttribute("scratchAmount", 0.0)));
        strip->setTransientSliceMode(stripXml->getBoolAttribute("transientSliceMode", false));

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

    if (auto* globalsXml = preset->getChildByName("Globals"))
    {
        if (auto* param = parameters.getParameter("masterVolume"))
            param->setValueNotifyingHost(static_cast<float>(globalsXml->getDoubleAttribute("masterVolume", 0.7)));
        if (auto* param = parameters.getParameter("quantize"))
            param->setValueNotifyingHost(globalsXml->getIntAttribute("quantize", 5) / 9.0f);
        if (auto* param = parameters.getParameter("crossfadeLength"))
        {
            const float crossfadeMs = static_cast<float>(globalsXml->getDoubleAttribute("crossfadeLength", 10.0));
            const float normalized = juce::jlimit(0.0f, 1.0f, (crossfadeMs - 1.0f) / 49.0f);
            param->setValueNotifyingHost(normalized);
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
