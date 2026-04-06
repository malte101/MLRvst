#include "PluginEditor.h"
#include "PluginEditorPanelUtils.h"
#include "PluginEditorStyle.h"
#include "mlrvst_build_info.h"
#include <cmath>
#include <limits>
#include <numeric>

using namespace PluginEditorStyle;
using namespace PluginEditorPanelUtils;

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

    headerFlipLabel.setText("Flip Mode Path", juce::dontSendNotification);
    headerFlipLabel.setColour(juce::Label::textColourId, kTextMuted);
    headerFlipLabel.setJustificationType(juce::Justification::centredLeft);
    scrollContent.addAndMakeVisible(headerFlipLabel);

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
        row.loopSetButton.setTooltip("Set pinned default loop-mode sample folder.");
        row.loopSetButton.onClick = [this, i]() { chooseDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Loop); };
        scrollContent.addAndMakeVisible(row.loopSetButton);

        row.loopClearButton.setButtonText("Clear");
        row.loopClearButton.setTooltip("Clear pinned default loop-mode folder.");
        row.loopClearButton.onClick = [this, i]() { clearDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Loop); };
        scrollContent.addAndMakeVisible(row.loopClearButton);

        row.stepPathLabel.setColour(juce::Label::textColourId, kTextPrimary);
        row.stepPathLabel.setJustificationType(juce::Justification::centredLeft);
        scrollContent.addAndMakeVisible(row.stepPathLabel);

        row.stepSetButton.setButtonText("Set");
        row.stepSetButton.setTooltip("Set pinned default step-mode sample folder.");
        row.stepSetButton.onClick = [this, i]() { chooseDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Step); };
        scrollContent.addAndMakeVisible(row.stepSetButton);

        row.stepClearButton.setButtonText("Clear");
        row.stepClearButton.setTooltip("Clear pinned default step-mode folder.");
        row.stepClearButton.onClick = [this, i]() { clearDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Step); };
        scrollContent.addAndMakeVisible(row.stepClearButton);

        row.flipPathLabel.setColour(juce::Label::textColourId, kTextPrimary);
        row.flipPathLabel.setJustificationType(juce::Justification::centredLeft);
        scrollContent.addAndMakeVisible(row.flipPathLabel);

        row.flipSetButton.setButtonText("Set");
        row.flipSetButton.setTooltip("Set pinned default flip-mode sample folder.");
        row.flipSetButton.onClick = [this, i]() { chooseDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Flip); };
        scrollContent.addAndMakeVisible(row.flipSetButton);

        row.flipClearButton.setButtonText("Clear");
        row.flipClearButton.setTooltip("Clear pinned default flip-mode folder.");
        row.flipClearButton.onClick = [this, i]() { clearDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Flip); };
        scrollContent.addAndMakeVisible(row.flipClearButton);
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
    const int pathAreaWidth = (header.getWidth() - stripWidth - (6 * buttonWidth) - (9 * gap)) / 3;

    headerStripLabel.setBounds(header.removeFromLeft(stripWidth));
    header.removeFromLeft(gap);
    headerLoopLabel.setBounds(header.removeFromLeft(pathAreaWidth + (2 * buttonWidth) + (2 * gap)));
    header.removeFromLeft(gap);
    headerStepLabel.setBounds(header.removeFromLeft(pathAreaWidth + (2 * buttonWidth) + (2 * gap)));
    header.removeFromLeft(gap);
    headerFlipLabel.setBounds(header);

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
        rowArea.removeFromLeft(gap * 2);

        row.flipPathLabel.setBounds(rowArea.removeFromLeft(pathAreaWidth));
        rowArea.removeFromLeft(gap);
        row.flipSetButton.setBounds(rowArea.removeFromLeft(buttonWidth));
        rowArea.removeFromLeft(gap);
        row.flipClearButton.setBounds(rowArea.removeFromLeft(buttonWidth));
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
        const auto flipDir = processor.getDefaultSampleDirectory(i, MlrVSTAudioProcessor::SamplePathMode::Flip);

        rows[idx].loopPathLabel.setText(pathToDisplay(loopDir), juce::dontSendNotification);
        rows[idx].loopPathLabel.setTooltip(loopDir.getFullPathName());
        rows[idx].stepPathLabel.setText(pathToDisplay(stepDir), juce::dontSendNotification);
        rows[idx].stepPathLabel.setTooltip(stepDir.getFullPathName());
        rows[idx].flipPathLabel.setText(pathToDisplay(flipDir), juce::dontSendNotification);
        rows[idx].flipPathLabel.setTooltip(flipDir.getFullPathName());
    }
}

void PathsControlPanel::chooseDirectory(int stripIndex, MlrVSTAudioProcessor::SamplePathMode mode)
{
    auto startDir = processor.getDefaultSampleDirectory(stripIndex, mode);
    if (!startDir.exists() || !startDir.isDirectory())
        startDir = processor.getCurrentBrowserDirectoryForStrip(stripIndex, mode);
    if (!startDir.exists() || !startDir.isDirectory())
        startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    juce::String modeName = "Loop";
    if (mode == MlrVSTAudioProcessor::SamplePathMode::Step)
        modeName = "Step";
    else if (mode == MlrVSTAudioProcessor::SamplePathMode::Flip)
        modeName = "Flip";
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
    if (file == juce::File() || file.getFullPathName().trim().isEmpty())
        return "(not set)";

    if (!file.exists() || !file.isDirectory())
        return file.getFullPathName() + " (missing)";

    return file.getFullPathName();
}

//==============================================================================
// SceneControlPanel Implementation
//==============================================================================

namespace
{
struct SceneAutomationLaneDefinition
{
    ScenePerformanceControlTarget target = ScenePerformanceControlTarget::None;
    const char* name = "";
    const char* hint = "";
    ModernAudioEngine::ModTarget primaryModTarget = ModernAudioEngine::ModTarget::None;
    bool grainOnly = false;
    bool bipolar = false;
    float defaultNormalizedValue = 0.5f;
};

constexpr std::array<SceneAutomationLaneDefinition, 26> kSceneAutomationLanes =
{{
    { ScenePerformanceControlTarget::Volume,            "Level",   "level",                 ModernAudioEngine::ModTarget::Volume,            false, false, 1.0f },
    { ScenePerformanceControlTarget::Pan,               "Pan",     "pan position",          ModernAudioEngine::ModTarget::Pan,               false, true,  0.5f },
    { ScenePerformanceControlTarget::Pitch,             "Pitch",   "pitch semitones",       ModernAudioEngine::ModTarget::Pitch,             false, true,  0.5f },
    { ScenePerformanceControlTarget::FilterFrequency,   "Cutoff",  "filter cutoff",         ModernAudioEngine::ModTarget::Cutoff,            false, false, 1.0f },
    { ScenePerformanceControlTarget::FilterResonance,   "Reso",    "filter resonance",      ModernAudioEngine::ModTarget::Resonance,         false, false, 0.0613131f },
    { ScenePerformanceControlTarget::FilterMorph,       "Morph",   "filter morph",          ModernAudioEngine::ModTarget::FilterMorph,       false, false, 0.0f },
    { ScenePerformanceControlTarget::Speed,             "Speed",   "speed ratio",           ModernAudioEngine::ModTarget::Speed,             false, true,  0.5f },
    { ScenePerformanceControlTarget::Retrigger,         "Stutter", "stutter depth",         ModernAudioEngine::ModTarget::Retrigger,         false, false, 0.0f },
    { ScenePerformanceControlTarget::SliceLength,       "Slice",   "slice length",          ModernAudioEngine::ModTarget::SliceLength,       false, false, 1.0f },
    { ScenePerformanceControlTarget::Scratch,           "Scratch", "scratch amount",        ModernAudioEngine::ModTarget::Scratch,           false, false, 0.0f },
    { ScenePerformanceControlTarget::DelayMix,          "D.Mix",   "delay mix",             ModernAudioEngine::ModTarget::DelayMix,          false, false, 0.0f },
    { ScenePerformanceControlTarget::DelayTime,         "D.Time",  "delay time",            ModernAudioEngine::ModTarget::DelayTime,         false, false, 0.0f },
    { ScenePerformanceControlTarget::DelayFeedback,     "D.Fbk",   "delay feedback",        ModernAudioEngine::ModTarget::DelayFeedback,     false, false, 0.0f },
    { ScenePerformanceControlTarget::GrainPitch,        "G.Pitch", "grain pitch",           ModernAudioEngine::ModTarget::GrainPitch,        true,  true,  0.5f },
    { ScenePerformanceControlTarget::GrainSize,         "G.Size",  "grain size",            ModernAudioEngine::ModTarget::GrainSize,         true,  false, 0.5156576f },
    { ScenePerformanceControlTarget::GrainDensity,      "G.Dns",   "grain density",         ModernAudioEngine::ModTarget::GrainDensity,      true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainPitchJitter,  "G.PJt",   "grain pitch jitter",    ModernAudioEngine::ModTarget::GrainPitchJitter,  true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainSpread,       "G.Spr",   "grain spread",          ModernAudioEngine::ModTarget::GrainSpread,       true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainJitter,       "G.Jit",   "grain jitter",          ModernAudioEngine::ModTarget::GrainJitter,       true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainPositionJitter, "G.Pos", "grain position jitter", ModernAudioEngine::ModTarget::GrainPositionJitter, true, false, 0.0f },
    { ScenePerformanceControlTarget::GrainRandomDepth,  "G.Rnd",   "grain random",          ModernAudioEngine::ModTarget::GrainRandom,       true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainArp,          "G.Arp",   "grain arp",             ModernAudioEngine::ModTarget::GrainArp,          true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainCloud,        "G.Cld",   "grain cloud",           ModernAudioEngine::ModTarget::GrainCloud,        true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainEmitter,      "G.Emt",   "grain emitter",         ModernAudioEngine::ModTarget::GrainEmitter,      true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainEnvelope,     "G.Env",   "grain envelope",        ModernAudioEngine::ModTarget::GrainEnvelope,     true,  false, 0.0f },
    { ScenePerformanceControlTarget::GrainShape,        "G.Shp",   "grain shape",           ModernAudioEngine::ModTarget::GrainShape,        true,  true,  0.5f }
}};

constexpr int kSceneAutomationLaneCount = static_cast<int>(kSceneAutomationLanes.size());
constexpr float kSceneCardGap = 8.0f;
constexpr float kSceneCardHeaderHeight = 22.0f;
constexpr float kSceneOverviewLaneHeight = 18.0f;
constexpr float kSceneRulerHeight = 16.0f;
constexpr float kSceneTriggerHeight = 54.0f;
constexpr float kSceneAutomationHeaderHeight = 18.0f;
constexpr float kSceneAutomationLaneHeight = 18.0f;
constexpr float kSceneAutomationMarkerSize = 5.0f;
constexpr float kSceneGlobalLaneExpandedScale = 3.0f;
constexpr float kSceneAutomationLaneGap = 2.0f;
constexpr float kSceneCardPaddingX = 10.0f;
constexpr float kSceneCardPaddingY = 8.0f;
constexpr float kSceneTimelineLabelWidth = 88.0f;
constexpr int kSceneStepColumnsPerRow = 16;
constexpr float kSceneStepLaunchHeight = 16.0f;
constexpr float kSceneStepPatternGap = 4.0f;
constexpr float kSceneStepRowHeight = 18.0f;
constexpr float kSceneStepExpandedRowHeight = 34.0f;

const SceneAutomationLaneDefinition& sceneAutomationLaneDefinition(int laneIndex)
{
    return kSceneAutomationLanes[static_cast<size_t>(juce::jlimit(0, kSceneAutomationLaneCount - 1, laneIndex))];
}

ScenePerformanceControlTarget sceneAutomationLaneTarget(int laneIndex)
{
    return sceneAutomationLaneDefinition(laneIndex).target;
}

int sceneAutomationLaneIndexForTarget(ScenePerformanceControlTarget target)
{
    for (int laneIndex = 0; laneIndex < kSceneAutomationLaneCount; ++laneIndex)
    {
        if (kSceneAutomationLanes[static_cast<size_t>(laneIndex)].target == target)
            return laneIndex;
    }

    return -1;
}

MlrVSTAudioProcessor::ControlMode sceneControlModeForTarget(ScenePerformanceControlTarget target)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
            return ControlMode::Speed;
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::GrainPitch:
            return ControlMode::Pitch;
        case ScenePerformanceControlTarget::Pan:
            return ControlMode::Pan;
        case ScenePerformanceControlTarget::Volume:
            return ControlMode::Volume;
        case ScenePerformanceControlTarget::Swing:
            return ControlMode::Swing;
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
            return ControlMode::GrainSize;
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
            return ControlMode::Filter;
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return ControlMode::Delay;
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::None:
        default:
            return ControlMode::Normal;
    }
}

struct SceneStripCardLayout
{
    juce::Rectangle<float> cardBounds;
    juce::Rectangle<float> headerBounds;
    juce::Rectangle<float> titleBounds;
    juce::Rectangle<float> summaryBounds;
    std::array<juce::Rectangle<float>, kSceneAutomationLaneCount> motionTargetBounds{};
    juce::Rectangle<float> stripWriteBounds;
    juce::Rectangle<float> stripWriteAllBounds;
    juce::Rectangle<float> stripClearBounds;
    juce::Rectangle<float> stripDuplicateBounds;
    juce::Rectangle<float> stripCopyBounds;
    juce::Rectangle<float> rulerLabelBounds;
    juce::Rectangle<float> rulerTimelineBounds;
    juce::Rectangle<float> triggerLabelBounds;
    juce::Rectangle<float> triggerTimelineBounds;
    juce::Rectangle<float> stepLaunchBounds;
    juce::Rectangle<float> stepPatternBounds;
    juce::Rectangle<float> automationHeaderBounds;
    juce::Rectangle<float> automationToggleBounds;
    std::array<juce::Rectangle<float>, kSceneAutomationLaneCount> automationLabelBounds{};
    std::array<juce::Rectangle<float>, kSceneAutomationLaneCount> automationTimelineBounds{};
    int stepTotalSteps = 0;
    int stepRowCount = 0;
    bool scenePlaybackAvailable = true;
    bool stepTriggerLane = false;
    bool automationExpanded = false;
    bool heightExpanded = false;
    bool compactOverview = false;
};

struct SceneGlobalLaneLayout
{
    juce::Rectangle<float> cardBounds;
    juce::Rectangle<float> headerBounds;
    juce::Rectangle<float> titleBounds;
    juce::Rectangle<float> summaryBounds;
    juce::Rectangle<float> laneLabelBounds;
    juce::Rectangle<float> laneBounds;
};

int sceneGlobalAutomationLaneIndex()
{
    return sceneAutomationLaneIndexForTarget(ScenePerformanceControlTarget::Retrigger);
}

bool sceneAutomationLaneUsesGlobalStrip(int laneIndex)
{
    return sceneAutomationLaneTarget(laneIndex) == ScenePerformanceControlTarget::Retrigger;
}

int sceneResolveAutomationStripIndex(int stripIndex, int laneIndex)
{
    if (sceneAutomationLaneUsesGlobalStrip(laneIndex) && stripIndex < 0)
        return -1;

    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
}

bool sceneIsGlobalAutomationEvent(const ScenePerformanceEvent& event)
{
    return event.type == ScenePerformanceEventType::ControlPoint
        && event.controlTarget == ScenePerformanceControlTarget::Retrigger;
}

bool sceneAutomationTargetUsesSteppedSegments(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainShape:
        default:
            return false;
    }
}

bool sceneAutomationEventUsesSteppedSegments(const ScenePerformanceEvent& event)
{
    return event.drawStepped || sceneAutomationTargetUsesSteppedSegments(event.controlTarget);
}

void drawSceneAutomationConnection(juce::Graphics& g,
                                   juce::Point<float> start,
                                   juce::Point<float> end,
                                   juce::Colour colour,
                                   bool stepped,
                                   float thickness)
{
    g.setColour(colour);
    if (stepped)
    {
        g.drawLine(start.x, start.y, end.x, start.y, thickness);
        g.drawLine(end.x, start.y, end.x, end.y, thickness);
        return;
    }

    g.drawLine(start.x, start.y, end.x, end.y, thickness);
}

void drawSceneHeaderActionChip(juce::Graphics& g,
                               juce::Rectangle<float> bounds,
                               const juce::String& label,
                               juce::Colour colour,
                               bool enabled);
bool sceneControlTargetMatchesModTarget(const MlrVSTAudioProcessor& processor,
                                        int stripIndex,
                                        ScenePerformanceControlTarget controlTarget,
                                        ModernAudioEngine::ModTarget target);

bool sceneStripUsesGrainLanes(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return false;

    auto* strip = engine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex));
    return strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain;
}

bool sceneStripSupportsScenePlayback(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    return processor.isStripScenePlaybackAvailable(stripIndex);
}

juce::String sceneStripPlaybackUnavailableReason(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return "Scene playback unavailable";

    auto* strip = engine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex));
    if (strip == nullptr)
        return "Scene playback unavailable";

    if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
        return "Load Flip audio to use scene playback";

    return "Scene playback unavailable";
}

bool sceneAutomationLaneVisible(const MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    const auto& lane = sceneAutomationLaneDefinition(laneIndex);
    if (sceneAutomationLaneUsesGlobalStrip(laneIndex))
        return stripIndex < 0;
    if (stripIndex < 0)
        return false;
    if (!sceneStripSupportsScenePlayback(processor, stripIndex))
        return false;
    return !lane.grainOnly || sceneStripUsesGrainLanes(processor, stripIndex);
}

int sceneAutomationVisibleLaneCount(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    int count = 0;
    for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
    {
        if (sceneAutomationLaneVisible(processor, stripIndex, lane))
            ++count;
    }
    return count;
}

juce::Colour sceneAutomationColour(const ScenePerformanceEvent& event)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (static_cast<ControlMode>(event.controlMode))
    {
        case ControlMode::Speed:     return juce::Colour(0xff6bbcff);
        case ControlMode::Pitch:     return juce::Colour(0xffff8f6b);
        case ControlMode::Pan:       return juce::Colour(0xff6ce0c4);
        case ControlMode::Volume:    return juce::Colour(0xff88d96b);
        case ControlMode::GrainSize: return juce::Colour(0xffffc86b);
        case ControlMode::Swing:     return juce::Colour(0xffffdd74);
        case ControlMode::Delay:     return juce::Colour(0xff74d6ff);
        case ControlMode::Filter:    return juce::Colour(0xffff9d5a);
        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:                     return kAccent;
    }
}

int sceneAutomationLaneIndex(const ScenePerformanceEvent& event)
{
    return sceneAutomationLaneIndexForTarget(event.controlTarget);
}

juce::String sceneAutomationLaneName(int laneIndex)
{
    return juce::String(sceneAutomationLaneDefinition(laneIndex).name);
}

juce::String sceneAutomationLaneCreateHint(int laneIndex)
{
    return juce::String(sceneAutomationLaneDefinition(laneIndex).hint);
}

std::array<juce::String, 3> sceneAutomationLaneScaleLabels(int laneIndex)
{
    const auto target = sceneAutomationLaneTarget(laneIndex);
    if (target == ScenePerformanceControlTarget::Speed)
        return { juce::String("8x"), juce::String("1x"), juce::String("1/8") };
    if (target == ScenePerformanceControlTarget::Pitch)
        return { juce::String("+24"), juce::String("0"), juce::String("-24") };
    if (target == ScenePerformanceControlTarget::GrainPitch)
        return { juce::String("+48"), juce::String("0"), juce::String("-48") };
    if (target == ScenePerformanceControlTarget::Pan)
        return { juce::String("R"), juce::String("C"), juce::String("L") };
    if (target == ScenePerformanceControlTarget::SliceLength)
        return { juce::String("Full"), juce::String("1/2"), juce::String("Min") };
    if (target == ScenePerformanceControlTarget::Retrigger)
        return { juce::String("Full"), juce::String(""), juce::String("Off") };
    if (target == ScenePerformanceControlTarget::FilterEnabled
        || target == ScenePerformanceControlTarget::DelaySyncEnabled)
    {
        return { juce::String("On"), juce::String(""), juce::String("Off") };
    }
    if (target == ScenePerformanceControlTarget::GrainShape)
        return { juce::String("+1"), juce::String("0"), juce::String("-1") };
    return {};
}

juce::String sceneLengthSummaryLabel(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int countValue = processor.getSceneLengthCount(sceneSlot);
    juce::ignoreUnused(processor, sceneSlot);
    return juce::String(countValue) + (countValue == 1 ? " Bar" : " Bars");
}

juce::String sceneControlTargetName(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:            return "Speed";
        case ScenePerformanceControlTarget::Pitch:            return "Pitch";
        case ScenePerformanceControlTarget::Pan:              return "Pan";
        case ScenePerformanceControlTarget::Volume:           return "Level";
        case ScenePerformanceControlTarget::Swing:            return "Swing";
        case ScenePerformanceControlTarget::GrainSize:        return "Grain Size";
        case ScenePerformanceControlTarget::GrainDensity:     return "Density";
        case ScenePerformanceControlTarget::GrainPitch:       return "Grain Pitch";
        case ScenePerformanceControlTarget::GrainPitchJitter: return "Pitch Jitter";
        case ScenePerformanceControlTarget::GrainSpread:      return "Spread";
        case ScenePerformanceControlTarget::GrainJitter:      return "Jitter";
        case ScenePerformanceControlTarget::GrainPositionJitter: return "Pos Jitter";
        case ScenePerformanceControlTarget::GrainRandomDepth: return "Random";
        case ScenePerformanceControlTarget::GrainArp:         return "Arp";
        case ScenePerformanceControlTarget::GrainCloud:       return "Cloud";
        case ScenePerformanceControlTarget::GrainEmitter:     return "Emitter";
        case ScenePerformanceControlTarget::GrainEnvelope:    return "Envelope";
        case ScenePerformanceControlTarget::GrainShape:       return "Shape";
        case ScenePerformanceControlTarget::FilterFrequency:  return "Cutoff";
        case ScenePerformanceControlTarget::FilterResonance:  return "Resonance";
        case ScenePerformanceControlTarget::FilterEnabled:    return "Filter";
        case ScenePerformanceControlTarget::FilterMorph:      return "Morph";
        case ScenePerformanceControlTarget::SliceLength:      return "Slice";
        case ScenePerformanceControlTarget::Scratch:          return "Scratch";
        case ScenePerformanceControlTarget::DelayMix:         return "Mix";
        case ScenePerformanceControlTarget::DelayTime:        return "Time";
        case ScenePerformanceControlTarget::DelayFeedback:    return "Feedback";
        case ScenePerformanceControlTarget::DelayLowCut:      return "Low Cut";
        case ScenePerformanceControlTarget::DelayHighCut:     return "High Cut";
        case ScenePerformanceControlTarget::DelayMode:        return "Mode";
        case ScenePerformanceControlTarget::DelaySyncEnabled: return "Sync";
        case ScenePerformanceControlTarget::Retrigger:        return "Stutter";
        case ScenePerformanceControlTarget::Rearrange:        return "Rearrange";
        case ScenePerformanceControlTarget::None:
        default:                                              return "Control";
    }
}

[[maybe_unused]] juce::String describeSceneEditorEvent(const ScenePerformanceEvent& event)
{
    if (event.type == ScenePerformanceEventType::Trigger)
        return "Pattern S" + juce::String(event.stripIndex + 1)
            + " @ " + juce::String(event.timeBeats, 2)
            + " • Col " + juce::String(juce::jlimit(0, 15, event.column) + 1);

    if (sceneIsGlobalAutomationEvent(event))
        return sceneControlTargetName(event.controlTarget)
            + " Global @ " + juce::String(event.timeBeats, 2);

    return sceneControlTargetName(event.controlTarget)
        + " S" + juce::String(event.stripIndex + 1)
        + " @ " + juce::String(event.timeBeats, 2);
}

bool sceneAutomationLaneIsBipolar(int laneIndex)
{
    return juce::isPositiveAndBelow(laneIndex, kSceneAutomationLaneCount)
        && sceneAutomationLaneDefinition(laneIndex).bipolar;
}

ModernAudioEngine::ModTarget scenePrimaryModTargetForLane(const MlrVSTAudioProcessor& processor,
                                                          int stripIndex,
                                                          int laneIndex)
{
    juce::ignoreUnused(processor, stripIndex);
    return sceneAutomationLaneDefinition(laneIndex).primaryModTarget;
}

bool sceneLaneMatchesModTarget(const MlrVSTAudioProcessor& processor,
                               int stripIndex,
                               int laneIndex,
                               ModernAudioEngine::ModTarget target)
{
    return sceneControlTargetMatchesModTarget(processor,
                                              stripIndex,
                                              sceneAutomationLaneTarget(laneIndex),
                                              target);
}

int sceneAssignedModSlotForLane(MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return -1;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        if (sceneLaneMatchesModTarget(processor,
                                      safeStripIndex,
                                      laneIndex,
                                      engine->getModTargetForSlot(safeStripIndex, slot)))
        {
            return slot;
        }
    }

    return -1;
}

bool sceneLaneCanShowMotionTargetSelector(MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    return sceneAutomationLaneVisible(processor, stripIndex, laneIndex);
}

ModernAudioEngine::ModTarget sceneDisplayedModTargetForLane(MlrVSTAudioProcessor& processor,
                                                            int stripIndex,
                                                            int laneIndex)
{
    const int assignedSlot = sceneAssignedModSlotForLane(processor, stripIndex, laneIndex);
    if (assignedSlot >= 0)
        return sanitizeModPerformanceTarget(processor.getSceneMotionTargetForSlot(stripIndex, assignedSlot));

    return sanitizeModPerformanceTarget(scenePrimaryModTargetForLane(processor, stripIndex, laneIndex));
}

juce::String sceneSlotDisplayName(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    return processor.getSceneInfo(sceneSlot).name;
}

juce::String sceneSlotShortDisplayName(const MlrVSTAudioProcessor& processor,
                                       int sceneSlot,
                                       int maxChars = 14)
{
    auto name = sceneSlotDisplayName(processor, sceneSlot);
    if (name.length() <= maxChars)
        return name;
    return name.substring(0, juce::jmax(0, maxChars - 3)).trimEnd() + "...";
}

std::vector<int> sceneActiveModSlotsForLane(MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    std::vector<int> slots;
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return slots;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        const auto target = engine->getModTargetForSlot(safeStripIndex, slot);
        const float depth = engine->getModDepthForSlot(safeStripIndex, slot);
        if (target == ModernAudioEngine::ModTarget::None || depth <= 0.001f)
            continue;
        if (sceneLaneMatchesModTarget(processor, safeStripIndex, laneIndex, target))
            slots.push_back(slot);
    }

    return slots;
}

enum class SceneLaneModControl
{
    None = 0,
    Depth,
    Rate,
    Clock,
    Length,
    Edit
};

struct SceneLaneModUiState
{
    int slot = -1;
    juce::Rectangle<float> depthBounds;
    juce::Rectangle<float> rateBounds;
    juce::Rectangle<float> clockBounds;
    juce::Rectangle<float> lengthBounds;
    juce::Rectangle<float> editBounds;
};

int scenePrimaryModSlotForLane(MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return -1;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const auto preferredTarget = scenePrimaryModTargetForLane(processor, safeStripIndex, laneIndex);
    const int activeSlot = juce::jlimit(0,
                                        ModernAudioEngine::NumModSequencers - 1,
                                        engine->getModSequencerSlot(safeStripIndex));
    const auto activeTarget = engine->getModTargetForSlot(safeStripIndex, activeSlot);
    if (activeTarget == preferredTarget
        || sceneLaneMatchesModTarget(processor, safeStripIndex, laneIndex, activeTarget))
    {
        return activeSlot;
    }

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        if (sceneLaneMatchesModTarget(processor,
                                      safeStripIndex,
                                      laneIndex,
                                      engine->getModTargetForSlot(safeStripIndex, slot)))
        {
            return slot;
        }
    }

    return -1;
}

int scenePreferredModEditorSlotForLane(MlrVSTAudioProcessor& processor, int stripIndex, int laneIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return -1;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const int safeLaneIndex = juce::jlimit(0, kSceneAutomationLaneCount - 1, laneIndex);
    const int primarySlot = scenePrimaryModSlotForLane(processor, safeStripIndex, safeLaneIndex);
    if (primarySlot >= 0)
        return primarySlot;

    const auto desiredTarget = sceneDisplayedModTargetForLane(processor, safeStripIndex, safeLaneIndex);
    if (desiredTarget == ModernAudioEngine::ModTarget::None)
        return -1;

    const int activeSlot = juce::jlimit(0,
                                        ModernAudioEngine::NumModSequencers - 1,
                                        engine->getModSequencerSlot(safeStripIndex));
    if (engine->getModTargetForSlot(safeStripIndex, activeSlot) == ModernAudioEngine::ModTarget::None)
        return activeSlot;

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        if (engine->getModTargetForSlot(safeStripIndex, slot) == ModernAudioEngine::ModTarget::None)
            return slot;
    }

    return activeSlot;
}

SceneLaneModUiState sceneLaneModUiState(MlrVSTAudioProcessor& processor,
                                        int stripIndex,
                                        int laneIndex,
                                        juce::Rectangle<float> laneBounds)
{
    SceneLaneModUiState state;
    state.slot = scenePreferredModEditorSlotForLane(processor, stripIndex, laneIndex);
    if (state.slot < 0 || laneBounds.isEmpty())
        return state;

    auto row = laneBounds.reduced(3.0f, 2.0f);
    row = juce::Rectangle<float>(row.getRight() - 28.0f,
                                 row.getY(),
                                 28.0f,
                                 juce::jmin(10.0f, row.getHeight()));
    state.editBounds = row;
    return state;
}

void drawSceneLaneModControls(juce::Graphics& g,
                              MlrVSTAudioProcessor& processor,
                              int stripIndex,
                              int laneIndex,
                              juce::Rectangle<float> laneBounds,
                              juce::Colour accent)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    const auto ui = sceneLaneModUiState(processor, stripIndex, laneIndex, laneBounds);
    if (ui.slot < 0)
        return;

    const auto chipColour = accent.withAlpha(0.9f);

    drawSceneHeaderActionChip(g, ui.editBounds, "Step", chipColour, true);
}

SceneLaneModControl hitSceneLaneModControl(MlrVSTAudioProcessor& processor,
                                           int stripIndex,
                                           int laneIndex,
                                           juce::Rectangle<float> laneBounds,
                                           juce::Point<float> position)
{
    const auto ui = sceneLaneModUiState(processor, stripIndex, laneIndex, laneBounds);
    if (ui.slot < 0)
        return SceneLaneModControl::None;
    if (ui.editBounds.expanded(2.0f, 1.0f).contains(position))
        return SceneLaneModControl::Edit;
    return SceneLaneModControl::None;
}

void cycleSceneLaneModControl(MlrVSTAudioProcessor& processor,
                              int stripIndex,
                              int laneIndex,
                              SceneLaneModControl control)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr || control == SceneLaneModControl::None || control == SceneLaneModControl::Edit)
        return;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const int slot = scenePrimaryModSlotForLane(processor, safeStripIndex, laneIndex);
    if (slot < 0)
        return;

    engine->setModSequencerSlot(safeStripIndex, slot);

    switch (control)
    {
        case SceneLaneModControl::Depth:
        {
            static constexpr std::array<float, 5> kDepthChoices{ 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            const float current = engine->getModDepth(safeStripIndex);
            int currentIndex = 0;
            float bestDelta = std::numeric_limits<float>::max();
            for (int i = 0; i < static_cast<int>(kDepthChoices.size()); ++i)
            {
                const float delta = std::abs(kDepthChoices[static_cast<size_t>(i)] - current);
                if (delta < bestDelta)
                {
                    bestDelta = delta;
                    currentIndex = i;
                }
            }
            engine->setModDepth(safeStripIndex,
                                kDepthChoices[static_cast<size_t>((currentIndex + 1) % static_cast<int>(kDepthChoices.size()))]);
            break;
        }

        case SceneLaneModControl::Rate:
        {
            const auto& rateChoices = PlayheadSpeedQuantizer::kSpeedRatios;
            const int currentIndex = PlayheadSpeedQuantizer::nearestSpeedIndex(engine->getModRate(safeStripIndex));
            const int nextIndex = (currentIndex + 1) % static_cast<int>(rateChoices.size());
            engine->setModRate(safeStripIndex, rateChoices[static_cast<size_t>(nextIndex)]);
            break;
        }

        case SceneLaneModControl::Clock:
        {
            const auto current = engine->getModTransportMode(safeStripIndex);
            const auto next = (current == ModernAudioEngine::ModTransportMode::Free)
                ? ModernAudioEngine::ModTransportMode::Scene
                : (current == ModernAudioEngine::ModTransportMode::Scene
                       ? ModernAudioEngine::ModTransportMode::Sync
                       : ModernAudioEngine::ModTransportMode::Free);
            engine->setModTransportMode(safeStripIndex, next);
            break;
        }

        case SceneLaneModControl::Length:
        {
            const int current = engine->getModLengthBars(safeStripIndex);
            engine->setModLengthBars(safeStripIndex,
                                     current >= ModernAudioEngine::MaxModBars ? 1 : (current + 1));
            break;
        }

        case SceneLaneModControl::Edit:
        case SceneLaneModControl::None:
        default:
            break;
    }

    if (processor.isSceneModeEnabled())
        processor.syncActiveSceneMotionState();
}

bool sceneStripUsesStepTriggerColumns(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    auto* engine = processor.getAudioEngine();
    auto* strip = engine != nullptr ? engine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex)) : nullptr;
    return strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
}

int sceneStepTotalSteps(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    auto* engine = processor.getAudioEngine();
    auto* strip = engine != nullptr ? engine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex)) : nullptr;
    return strip != nullptr ? juce::jmax(1, strip->getStepTotalSteps()) : kSceneStepColumnsPerRow;
}

int sceneStepRowCount(int totalSteps)
{
    return juce::jmax(1, (juce::jmax(1, totalSteps) + (kSceneStepColumnsPerRow - 1)) / kSceneStepColumnsPerRow);
}

float sceneTriggerLaneHeight(const MlrVSTAudioProcessor& processor, int stripIndex, bool heightExpanded)
{
    const float verticalScale = heightExpanded ? 2.55f : 1.0f;
    const float defaultHeight = kSceneTriggerHeight * verticalScale;
    if (!sceneStripUsesStepTriggerColumns(processor, stripIndex))
        return defaultHeight;

    const int rows = sceneStepRowCount(sceneStepTotalSteps(processor, stripIndex));
    const float launchHeight = kSceneStepLaunchHeight * (heightExpanded ? 1.35f : 1.0f);
    const float patternGap = kSceneStepPatternGap * (heightExpanded ? 1.25f : 1.0f);
    const float rowHeight = heightExpanded ? kSceneStepExpandedRowHeight : kSceneStepRowHeight;
    return juce::jmax(defaultHeight, launchHeight + patternGap + (static_cast<float>(rows) * rowHeight) + 4.0f);
}

juce::Rectangle<float> sceneStepCellBounds(const SceneStripCardLayout& layout, int stepIndex)
{
    if (!layout.stepTriggerLane || layout.stepPatternBounds.isEmpty() || layout.stepRowCount <= 0)
        return {};

    const int safeIndex = juce::jlimit(0, juce::jmax(0, layout.stepTotalSteps - 1), stepIndex);
    const int row = safeIndex / kSceneStepColumnsPerRow;
    const int column = safeIndex % kSceneStepColumnsPerRow;
    const float cellWidth = layout.stepPatternBounds.getWidth() / static_cast<float>(kSceneStepColumnsPerRow);
    const float cellHeight = layout.stepPatternBounds.getHeight() / static_cast<float>(juce::jmax(1, layout.stepRowCount));
    return juce::Rectangle<float>(layout.stepPatternBounds.getX() + (cellWidth * static_cast<float>(column)),
                                  layout.stepPatternBounds.getY() + (cellHeight * static_cast<float>(row)),
                                  cellWidth,
                                  cellHeight)
        .reduced(1.0f, 1.0f);
}

int sceneStepAbsoluteStepAtPosition(const SceneStripCardLayout& layout, juce::Point<float> position)
{
    if (!layout.stepTriggerLane || !layout.stepPatternBounds.contains(position) || layout.stepRowCount <= 0)
        return -1;

    const float cellWidth = layout.stepPatternBounds.getWidth() / static_cast<float>(kSceneStepColumnsPerRow);
    const float cellHeight = layout.stepPatternBounds.getHeight() / static_cast<float>(juce::jmax(1, layout.stepRowCount));
    const int column = juce::jlimit(0,
                                    kSceneStepColumnsPerRow - 1,
                                    static_cast<int>((position.x - layout.stepPatternBounds.getX()) / juce::jmax(1.0f, cellWidth)));
    const int row = juce::jlimit(0,
                                 juce::jmax(0, layout.stepRowCount - 1),
                                 static_cast<int>((position.y - layout.stepPatternBounds.getY()) / juce::jmax(1.0f, cellHeight)));
    const int absoluteStep = (row * kSceneStepColumnsPerRow) + column;
    return absoluteStep < layout.stepTotalSteps ? absoluteStep : -1;
}

juce::String sceneTriggerLaneSummaryText(const MlrVSTAudioProcessor& processor,
                                         int stripIndex,
                                         bool heightExpanded,
                                         const juce::String& sceneLengthLabel)
{
    if (!sceneStripSupportsScenePlayback(processor, stripIndex))
        return "Scene playback unavailable";

    if (!sceneStripUsesStepTriggerColumns(processor, stripIndex))
        return (heightExpanded ? "Pattern / Offset  x2  •  " : "Pattern / Offset  •  ") + sceneLengthLabel;

    const int totalSteps = sceneStepTotalSteps(processor, stripIndex);
    return (heightExpanded ? "Step Grid / Launch  x2  •  " : "Step Grid / Launch  •  ")
        + juce::String(totalSteps) + " steps  •  " + sceneLengthLabel;
}

float sceneShapeModCurvePhase(float phase01, float bend, ModernAudioEngine::ModCurveShape shape)
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

float sceneShapeSubdivisionBendPhase(float phase01, float bend)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);
    const float exp = 1.0f + (18.0f * amount);
    return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
}

float sceneSampleStepSubdivisionValue(float startValue, float endValue, int subdivisions, float stepPhase01)
{
    const float start = juce::jlimit(0.0f, 1.0f, startValue);
    const float end = juce::jlimit(0.0f, 1.0f, endValue);
    const int subdiv = juce::jlimit(1, ModernAudioEngine::ModMaxStepSubdivisions, subdivisions);
    const float phase = juce::jlimit(0.0f, 0.999999f, stepPhase01);

    if (subdiv <= 1)
        return start;

    const float subdivPos = phase * static_cast<float>(subdiv);
    const int subdivIndex = juce::jlimit(0, subdiv - 1, static_cast<int>(std::floor(subdivPos)));
    const float t = static_cast<float>(subdivIndex) / static_cast<float>(juce::jmax(1, subdiv - 1));
    return juce::jlimit(0.0f, 1.0f, start + ((end - start) * t));
}

double sceneWrapModPosition(double position, int totalSteps)
{
    if (totalSteps <= 0)
        return 0.0;

    const double wrapped = std::fmod(position, static_cast<double>(totalSteps));
    return wrapped < 0.0 ? (wrapped + static_cast<double>(totalSteps)) : wrapped;
}

float sceneSampleModSlotRawValue(const ModernAudioEngine& engine, int stripIndex, int slot, double stepPosition)
{
    const int lengthBars = juce::jmax(1, engine.getModLengthBarsForSlot(stripIndex, slot));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps,
                                      ModernAudioEngine::ModSteps * lengthBars);
    const double wrappedPos = sceneWrapModPosition(stepPosition, totalSteps);
    const int globalStep = juce::jlimit(0, totalSteps - 1, static_cast<int>(std::floor(wrappedPos)));
    const float phase = static_cast<float>(juce::jlimit(0.0, 1.0, wrappedPos - static_cast<double>(globalStep)));
    const int nextStep = (globalStep + 1) % totalSteps;

    const float startA = engine.getModStepValueAbsoluteForSlot(stripIndex, slot, globalStep);
    const int subdivisionsA = engine.getModStepSubdivisionAbsoluteForSlot(stripIndex, slot, globalStep);
    const float endA = engine.getModStepEndValueAbsoluteForSlot(stripIndex, slot, globalStep);
    const float nextStart = engine.getModStepValueAbsoluteForSlot(stripIndex, slot, nextStep);
    const float curveBend = engine.getModCurveBendForSlot(stripIndex, slot);
    const bool curveMode = engine.isModCurveModeForSlot(stripIndex, slot);
    const auto curveShape = engine.getModStepCurveShapeAbsoluteForSlot(stripIndex, slot, globalStep);
    const bool hasLocalSubdivisionRamp = subdivisionsA > 1;

    const float shapedPhase = curveMode
        ? sceneShapeModCurvePhase(phase, curveBend, curveShape)
        : phase;
    const float subdivisionPhase = (curveMode && hasLocalSubdivisionRamp)
        ? sceneShapeSubdivisionBendPhase(phase, curveBend)
        : phase;
    const float rawA = sceneSampleStepSubdivisionValue(startA, endA, subdivisionsA, subdivisionPhase);

    if (curveMode && !hasLocalSubdivisionRamp)
        return juce::jlimit(0.0f, 1.0f, startA + ((nextStart - startA) * shapedPhase));

    return juce::jlimit(0.0f, 1.0f, rawA);
}

juce::Colour sceneModPreviewColour(juce::Colour laneTint, int slot, bool followStrip)
{
    const auto base = juce::Colour(0xffd9a04a)
        .interpolatedWith(laneTint, 0.18f + (0.12f * static_cast<float>(slot % 3)));
    return base.withMultipliedBrightness(followStrip ? 1.08f : 1.0f);
}

void drawSceneModLanePreview(juce::Graphics& g,
                             MlrVSTAudioProcessor& processor,
                             int stripIndex,
                             int laneIndex,
                             juce::Rectangle<float> laneBounds,
                             double sceneLengthBeats,
                             juce::Colour laneTint)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr || laneBounds.isEmpty())
        return;

    const auto modSlots = sceneActiveModSlotsForLane(processor, stripIndex, laneIndex);
    if (modSlots.empty())
        return;

    const float normHeight = juce::jmax(4.0f, laneBounds.getHeight() - 4.0f);
    const float bottomY = laneBounds.getBottom() - 2.0f;

    for (size_t slotIndex = 0; slotIndex < modSlots.size(); ++slotIndex)
    {
        const int slot = modSlots[slotIndex];
        const float depth = engine->getModDepthForSlot(stripIndex, slot);
        if (depth <= 0.001f)
            continue;

        const auto transportMode = engine->getModTransportModeForSlot(stripIndex, slot);
        const float rate = juce::jmax(0.125f, engine->getModRateForSlot(stripIndex, slot));
        const int offset = engine->getModOffsetForSlot(stripIndex, slot);
        const int lengthBars = juce::jmax(1, engine->getModLengthBarsForSlot(stripIndex, slot));
        const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps,
                                          ModernAudioEngine::ModSteps * lengthBars);
        const bool followStrip = transportMode == ModernAudioEngine::ModTransportMode::Sync;
        const bool followScene = transportMode == ModernAudioEngine::ModTransportMode::Scene;
        auto previewBounds = laneBounds.reduced(2.0f, 2.0f);
        const double cycleBeats = followScene
            ? juce::jmax(1.0e-6, sceneLengthBeats / static_cast<double>(rate))
            : juce::jmax(1.0e-6, (4.0 * static_cast<double>(lengthBars)) / static_cast<double>(rate));

        if (followStrip)
        {
            const double visibleBeats = juce::jmax(1.0, sceneLengthBeats);
            const float normalizedCycleWidth = static_cast<float>(juce::jlimit(0.0,
                                                                               1.0,
                                                                               cycleBeats / visibleBeats));
            const float motifWidth = juce::jlimit(44.0f,
                                                  juce::jmax(44.0f, previewBounds.getWidth()),
                                                  previewBounds.getWidth() * juce::jmax(0.12f, normalizedCycleWidth));
            previewBounds.setWidth(motifWidth);
        }

        const int sampleCount = juce::jlimit(24,
                                             192,
                                             static_cast<int>(std::ceil(previewBounds.getWidth() / 3.0f)));
        if (sampleCount < 2)
            continue;

        if (cycleBeats < (sceneLengthBeats - 1.0e-6))
        {
            for (double beat = cycleBeats; beat < sceneLengthBeats; beat += cycleBeats)
            {
                const float x = laneBounds.getX()
                    + (laneBounds.getWidth() * static_cast<float>(beat / juce::jmax(1.0, sceneLengthBeats)));
                g.setColour(laneTint.withAlpha(0.12f));
                g.drawVerticalLine(static_cast<int>(std::round(x)),
                                   previewBounds.getY() + 1.0f,
                                   previewBounds.getBottom() - 1.0f);
            }
        }

        juce::Path path;
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            const float u = static_cast<float>(sampleIndex) / static_cast<float>(sampleCount - 1);
            double stepPosition = 0.0;
            if (followStrip)
            {
                stepPosition = (static_cast<double>(u) * static_cast<double>(ModernAudioEngine::ModSteps) * static_cast<double>(rate))
                    + static_cast<double>(offset);
            }
            else if (followScene)
            {
                const double scenePhase = static_cast<double>(u);
                stepPosition = ((scenePhase * static_cast<double>(totalSteps)) * static_cast<double>(rate))
                    + static_cast<double>(offset);
            }
            else
            {
                const double beat = static_cast<double>(u) * juce::jmax(1.0, sceneLengthBeats);
                stepPosition = (beat * 4.0 * static_cast<double>(rate)) + static_cast<double>(offset);
            }

            const float raw = sceneSampleModSlotRawValue(*engine, stripIndex, slot, stepPosition);
            const float x = previewBounds.getX() + (previewBounds.getWidth() * u);
            const float y = bottomY - (raw * normHeight);
            if (sampleIndex == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }

        const auto colour = sceneModPreviewColour(laneTint, slot, followStrip)
            .withAlpha(juce::jlimit(0.16f, 0.72f, 0.22f + (depth * 0.34f)));
        g.setColour(colour);

        if (followStrip)
        {
            const float repeatSpacing = juce::jmax(1.0f, previewBounds.getWidth());
            const int repeatCount = juce::jmax(1,
                                               juce::jmin(32,
                                                          static_cast<int>(std::ceil(laneBounds.getWidth() / repeatSpacing))));
            for (int repeat = 0; repeat < repeatCount; ++repeat)
            {
                const float alphaScale = repeat == 0 ? 1.0f : (repeat == 1 ? 0.78f : 0.56f);
                g.setColour(colour.withAlpha(colour.getFloatAlpha() * alphaScale));
                g.strokePath(path,
                             juce::PathStrokeType(repeat == 0 ? 1.4f : 1.0f),
                             juce::AffineTransform::translation(repeatSpacing * static_cast<float>(repeat), 0.0f));
            }
        }
        else
        {
            g.strokePath(path, juce::PathStrokeType(1.25f));
        }
    }
}

bool sceneControlTargetMatchesModTarget(const MlrVSTAudioProcessor& processor,
                                        int stripIndex,
                                        ScenePerformanceControlTarget controlTarget,
                                        ModernAudioEngine::ModTarget target)
{
    using ModTarget = ModernAudioEngine::ModTarget;
    const bool grainMode = sceneStripUsesGrainLanes(processor, stripIndex);

    switch (controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:            return target == ModTarget::Speed;
        case ScenePerformanceControlTarget::Pitch:
            return target == ModTarget::Pitch || (grainMode && target == ModTarget::GrainPitch);
        case ScenePerformanceControlTarget::Pan:              return target == ModTarget::Pan;
        case ScenePerformanceControlTarget::Volume:           return target == ModTarget::Volume;
        case ScenePerformanceControlTarget::GrainSize:        return target == ModTarget::GrainSize;
        case ScenePerformanceControlTarget::GrainDensity:     return target == ModTarget::GrainDensity;
        case ScenePerformanceControlTarget::GrainPitchJitter: return target == ModTarget::GrainPitchJitter;
        case ScenePerformanceControlTarget::GrainSpread:      return target == ModTarget::GrainSpread;
        case ScenePerformanceControlTarget::GrainJitter:      return target == ModTarget::GrainJitter;
        case ScenePerformanceControlTarget::GrainPositionJitter: return target == ModTarget::GrainPositionJitter;
        case ScenePerformanceControlTarget::GrainRandomDepth: return target == ModTarget::GrainRandom;
        case ScenePerformanceControlTarget::GrainArp:         return target == ModTarget::GrainArp;
        case ScenePerformanceControlTarget::GrainCloud:       return target == ModTarget::GrainCloud;
        case ScenePerformanceControlTarget::GrainEmitter:     return target == ModTarget::GrainEmitter;
        case ScenePerformanceControlTarget::GrainEnvelope:    return target == ModTarget::GrainEnvelope;
        case ScenePerformanceControlTarget::GrainShape:       return target == ModTarget::GrainShape;
        case ScenePerformanceControlTarget::GrainPitch:
            return target == ModTarget::GrainPitch || target == ModTarget::Pitch;
        case ScenePerformanceControlTarget::FilterFrequency:  return target == ModTarget::Cutoff;
        case ScenePerformanceControlTarget::FilterResonance:  return target == ModTarget::Resonance;
        case ScenePerformanceControlTarget::Retrigger:        return target == ModTarget::Retrigger;
        case ScenePerformanceControlTarget::Rearrange:        return target == ModTarget::Rearrange;
        case ScenePerformanceControlTarget::FilterMorph:      return target == ModTarget::FilterMorph;
        case ScenePerformanceControlTarget::SliceLength:      return target == ModTarget::SliceLength;
        case ScenePerformanceControlTarget::Scratch:          return target == ModTarget::Scratch;
        case ScenePerformanceControlTarget::DelayMix:         return target == ModTarget::DelayMix;
        case ScenePerformanceControlTarget::DelayTime:        return target == ModTarget::DelayTime;
        case ScenePerformanceControlTarget::DelayFeedback:    return target == ModTarget::DelayFeedback;
        case ScenePerformanceControlTarget::DelayLowCut:      return target == ModTarget::DelayLowCut;
        case ScenePerformanceControlTarget::DelayHighCut:     return target == ModTarget::DelayHighCut;
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::None:
        default:
            return false;
    }
}

float normalizeSceneAutomationValue(const ScenePerformanceEvent& event)
{
    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
        {
            const float safeValue = juce::jlimit(0.125f, 8.0f, event.value);
            return juce::jlimit(0.0f, 1.0f, (std::log2(safeValue) + 3.0f) / 6.0f);
        }
        case ScenePerformanceControlTarget::Pitch:
            return juce::jlimit(0.0f, 1.0f, (event.value + 24.0f) / 48.0f);
        case ScenePerformanceControlTarget::GrainPitch:
            return juce::jlimit(0.0f, 1.0f, (event.value + 48.0f) / 96.0f);
        case ScenePerformanceControlTarget::Pan:
            return juce::jlimit(0.0f, 1.0f, (event.value + 1.0f) * 0.5f);
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return juce::jlimit(0.0f, 1.0f, event.value);
        case ScenePerformanceControlTarget::SliceLength:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.02f) / 0.98f);
        case ScenePerformanceControlTarget::Scratch:
            return juce::jlimit(0.0f, 1.0f, event.value / 100.0f);
        case ScenePerformanceControlTarget::GrainSize:
            return juce::jlimit(0.0f, 1.0f, (event.value - 5.0f) / (2400.0f - 5.0f));
        case ScenePerformanceControlTarget::GrainDensity:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.05f) / (0.9f - 0.05f));
        case ScenePerformanceControlTarget::GrainPitchJitter:
            return juce::jlimit(0.0f, 1.0f, event.value / 48.0f);
        case ScenePerformanceControlTarget::GrainShape:
            return juce::jlimit(0.0f, 1.0f, (event.value + 1.0f) * 0.5f);
        case ScenePerformanceControlTarget::FilterFrequency:
        {
            const float safeValue = juce::jlimit(20.0f, 20000.0f, event.value);
            return juce::jlimit(0.0f, 1.0f, std::log(safeValue / 20.0f) / std::log(1000.0f));
        }
        case ScenePerformanceControlTarget::FilterResonance:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.1f) / 9.9f);
        case ScenePerformanceControlTarget::DelayTime:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.25f) / (4.0f - 0.25f));
        case ScenePerformanceControlTarget::DelayFeedback:
            return juce::jlimit(0.0f, 1.0f, event.value / 0.97f);
        case ScenePerformanceControlTarget::DelayLowCut:
        {
            const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(event.value));
        }
        case ScenePerformanceControlTarget::DelayHighCut:
        {
            const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(event.value));
        }
        case ScenePerformanceControlTarget::DelayMode:
            return juce::jlimit(0.0f, 1.0f, event.value / 2.0f);
        case ScenePerformanceControlTarget::None:
        default:
            return 0.5f;
    }
}

float denormalizeSceneAutomationValue(const ScenePerformanceEvent& event, float normalizedValue)
{
    const float t = juce::jlimit(0.0f, 1.0f, normalizedValue);

    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            return juce::jlimit(0.125f, 8.0f, std::pow(2.0f, -3.0f + (t * 6.0f)));
        case ScenePerformanceControlTarget::Pitch:
            return -24.0f + (t * 48.0f);
        case ScenePerformanceControlTarget::GrainPitch:
            return -48.0f + (t * 96.0f);
        case ScenePerformanceControlTarget::Pan:
            return (t * 2.0f) - 1.0f;
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
            return t;
        case ScenePerformanceControlTarget::SliceLength:
            return 0.02f + (t * 0.98f);
        case ScenePerformanceControlTarget::Scratch:
            return 100.0f * t;
        case ScenePerformanceControlTarget::GrainSize:
            return 5.0f + (t * (2400.0f - 5.0f));
        case ScenePerformanceControlTarget::GrainDensity:
            return 0.05f + (t * (0.9f - 0.05f));
        case ScenePerformanceControlTarget::GrainPitchJitter:
            return 48.0f * t;
        case ScenePerformanceControlTarget::GrainShape:
            return (t * 2.0f) - 1.0f;
        case ScenePerformanceControlTarget::FilterFrequency:
            return 20.0f * std::pow(1000.0f, t);
        case ScenePerformanceControlTarget::FilterResonance:
            return 0.1f + (t * 9.9f);
        case ScenePerformanceControlTarget::DelayTime:
            return 0.25f + (t * (4.0f - 0.25f));
        case ScenePerformanceControlTarget::DelayFeedback:
            return 0.97f * t;
        case ScenePerformanceControlTarget::DelayLowCut:
        {
            const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
            return range.convertFrom0to1(t);
        }
        case ScenePerformanceControlTarget::DelayHighCut:
        {
            const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
            return range.convertFrom0to1(t);
        }
        case ScenePerformanceControlTarget::DelayMode:
            return static_cast<float>(juce::jlimit(0, 2, static_cast<int>(std::round(t * 2.0f))));
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return t >= 0.5f ? 1.0f : 0.0f;
        case ScenePerformanceControlTarget::None:
        default:
            return event.value;
    }
}

juce::String sceneControlTargetShortName(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:            return "Spd";
        case ScenePerformanceControlTarget::Pitch:            return "Pit";
        case ScenePerformanceControlTarget::Pan:              return "Pan";
        case ScenePerformanceControlTarget::Volume:           return "Lvl";
        case ScenePerformanceControlTarget::Swing:            return "Swg";
        case ScenePerformanceControlTarget::GrainSize:        return "Size";
        case ScenePerformanceControlTarget::GrainDensity:     return "Dns";
        case ScenePerformanceControlTarget::GrainPitch:       return "GPt";
        case ScenePerformanceControlTarget::GrainPitchJitter: return "PJt";
        case ScenePerformanceControlTarget::GrainSpread:      return "Spr";
        case ScenePerformanceControlTarget::GrainJitter:      return "Jit";
        case ScenePerformanceControlTarget::GrainPositionJitter: return "Pos";
        case ScenePerformanceControlTarget::GrainRandomDepth: return "Rnd";
        case ScenePerformanceControlTarget::GrainArp:         return "Arp";
        case ScenePerformanceControlTarget::GrainCloud:       return "Cld";
        case ScenePerformanceControlTarget::GrainEmitter:     return "Emt";
        case ScenePerformanceControlTarget::GrainEnvelope:    return "Env";
        case ScenePerformanceControlTarget::GrainShape:       return "Shp";
        case ScenePerformanceControlTarget::FilterFrequency:  return "Cut";
        case ScenePerformanceControlTarget::FilterResonance:  return "Res";
        case ScenePerformanceControlTarget::FilterEnabled:    return "On";
        case ScenePerformanceControlTarget::FilterMorph:      return "Mrf";
        case ScenePerformanceControlTarget::SliceLength:      return "Slc";
        case ScenePerformanceControlTarget::Scratch:          return "Scr";
        case ScenePerformanceControlTarget::DelayMix:         return "Mix";
        case ScenePerformanceControlTarget::DelayTime:        return "Tim";
        case ScenePerformanceControlTarget::DelayFeedback:    return "Fbk";
        case ScenePerformanceControlTarget::DelayLowCut:      return "Low";
        case ScenePerformanceControlTarget::DelayHighCut:     return "Hi";
        case ScenePerformanceControlTarget::DelayMode:        return "Dly";
        case ScenePerformanceControlTarget::DelaySyncEnabled: return "Syn";
        case ScenePerformanceControlTarget::Retrigger:        return "Stu";
        case ScenePerformanceControlTarget::Rearrange:        return "Rng";
        case ScenePerformanceControlTarget::None:
        default:                                              return "Ctl";
    }
}

bool sceneCurrentEffectiveNormalizedValue(MlrVSTAudioProcessor& processor,
                                          int stripIndex,
                                          ScenePerformanceControlTarget target,
                                          float& normalizedOut)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return false;

    float value = 0.0f;
    switch (target)
    {
        case ScenePerformanceControlTarget::Retrigger:
            value = processor.getGlobalSceneStutterAmount();
            break;
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Rearrange:
        {
            if (stripIndex < 0)
                return false;

            const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
            auto* strip = engine->getStrip(safeStripIndex);
            if (strip == nullptr)
                return false;

            switch (target)
            {
                case ScenePerformanceControlTarget::Speed:
                    value = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
                        ? strip->getPlaybackSpeed()
                        : strip->getPlayheadSpeedRatio();
                    break;
                case ScenePerformanceControlTarget::Pitch:
                    value = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
                        ? strip->getGrainPitch()
                        : processor.getPitchSemitonesForDisplay(*strip);
                    break;
                case ScenePerformanceControlTarget::Pan:
                    value = strip->getPan();
                    break;
                case ScenePerformanceControlTarget::Volume:
                    value = strip->getVolume();
                    break;
                case ScenePerformanceControlTarget::Swing:
                    value = strip->getSwingAmount();
                    break;
                case ScenePerformanceControlTarget::GrainSize:
                    value = strip->getGrainSizeMs();
                    break;
                case ScenePerformanceControlTarget::GrainDensity:
                    value = strip->getGrainDensity();
                    break;
                case ScenePerformanceControlTarget::GrainPitch:
                    value = strip->getGrainPitch();
                    break;
                case ScenePerformanceControlTarget::GrainPitchJitter:
                    value = strip->getGrainPitchJitter();
                    break;
                case ScenePerformanceControlTarget::GrainSpread:
                    value = strip->getGrainSpread();
                    break;
                case ScenePerformanceControlTarget::GrainJitter:
                    value = strip->getGrainJitter();
                    break;
                case ScenePerformanceControlTarget::GrainPositionJitter:
                    value = strip->getGrainPositionJitter();
                    break;
                case ScenePerformanceControlTarget::GrainRandomDepth:
                    value = strip->getGrainRandomDepth();
                    break;
                case ScenePerformanceControlTarget::GrainArp:
                    value = strip->getGrainArpDepth();
                    break;
                case ScenePerformanceControlTarget::GrainCloud:
                    value = strip->getGrainCloudDepth();
                    break;
                case ScenePerformanceControlTarget::GrainEmitter:
                    value = strip->getGrainEmitterDepth();
                    break;
                case ScenePerformanceControlTarget::GrainEnvelope:
                    value = strip->getGrainEnvelope();
                    break;
                case ScenePerformanceControlTarget::GrainShape:
                    value = strip->getGrainShape();
                    break;
                case ScenePerformanceControlTarget::FilterFrequency:
                    value = strip->getFilterFrequency();
                    break;
                case ScenePerformanceControlTarget::FilterResonance:
                    value = strip->getFilterResonance();
                    break;
                case ScenePerformanceControlTarget::FilterEnabled:
                    value = strip->isFilterEnabled() ? 1.0f : 0.0f;
                    break;
                case ScenePerformanceControlTarget::FilterMorph:
                    value = strip->getFilterMorph();
                    break;
                case ScenePerformanceControlTarget::SliceLength:
                    value = strip->getLoopSliceLength();
                    break;
                case ScenePerformanceControlTarget::Scratch:
                    value = strip->getScratchAmount();
                    break;
                case ScenePerformanceControlTarget::DelayMix:
                    value = strip->getDelayMix();
                    break;
                case ScenePerformanceControlTarget::DelayTime:
                    value = strip->getDelayTimeBeats();
                    break;
                case ScenePerformanceControlTarget::DelayFeedback:
                    value = strip->getDelayFeedback();
                    break;
                case ScenePerformanceControlTarget::DelayLowCut:
                    value = strip->getDelayLowCutHz();
                    break;
                case ScenePerformanceControlTarget::DelayHighCut:
                    value = strip->getDelayHighCutHz();
                    break;
                case ScenePerformanceControlTarget::DelayMode:
                    value = static_cast<float>(static_cast<int>(strip->getDelayMode()));
                    break;
                case ScenePerformanceControlTarget::DelaySyncEnabled:
                    value = strip->isDelaySyncEnabled() ? 1.0f : 0.0f;
                    break;
                case ScenePerformanceControlTarget::Rearrange:
                    value = strip->isTraversalRearrangeActive() ? strip->getTraversalRearrangeValue() : 0.0f;
                    break;
                case ScenePerformanceControlTarget::Retrigger:
                case ScenePerformanceControlTarget::None:
                default:
                    break;
            }
            break;
        }
        case ScenePerformanceControlTarget::None:
        default:
            return false;
    }

    ScenePerformanceEvent probe;
    probe.type = ScenePerformanceEventType::ControlPoint;
    probe.controlTarget = target;
    probe.value = value;
    normalizedOut = normalizeSceneAutomationValue(probe);
    return std::isfinite(normalizedOut);
}

bool sceneCurrentBaseNormalizedValue(MlrVSTAudioProcessor& processor,
                                     int stripIndex,
                                     ScenePerformanceControlTarget target,
                                     float& normalizedOut)
{
    return processor.getSceneControlBaseNormalizedValue(stripIndex, target, normalizedOut);
}

bool sceneHeldNormalizedValueForTarget(const std::vector<ScenePerformanceEvent>& events,
                                       int stripIndex,
                                       ScenePerformanceControlTarget target,
                                       double beat,
                                       double lengthBeats,
                                       float& normalizedOut)
{
    const int safeStripIndex = (stripIndex < 0) ? -1 : juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const double safeBeat = juce::jlimit(0.0, std::nextafter(juce::jmax(1.0, lengthBeats), 0.0), beat);
    const ScenePerformanceEvent* lastEvent = nullptr;
    const ScenePerformanceEvent* chosenEvent = nullptr;

    for (const auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::ControlPoint
            || event.controlTarget != target)
        {
            continue;
        }

        if (!(target == ScenePerformanceControlTarget::Retrigger && safeStripIndex < 0)
            && event.stripIndex != safeStripIndex)
        {
            continue;
        }

        lastEvent = &event;
        if (event.timeBeats <= safeBeat + 1.0e-6)
            chosenEvent = &event;
    }

    if (chosenEvent == nullptr)
        chosenEvent = lastEvent;
    if (chosenEvent == nullptr)
        return false;

    normalizedOut = normalizeSceneAutomationValue(*chosenEvent);
    return std::isfinite(normalizedOut);
}

std::vector<ScenePerformanceControlTarget> sceneOverlappingTargetsForLane(MlrVSTAudioProcessor& processor,
                                                                          const std::vector<ScenePerformanceEvent>& events,
                                                                          int stripIndex,
                                                                          int laneIndex)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return {};

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    std::vector<ScenePerformanceControlTarget> targets;
    for (const auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::ControlPoint
            || event.stripIndex != safeStripIndex
            || sceneAutomationLaneIndex(event) != laneIndex)
        {
            continue;
        }

        if (std::find(targets.begin(), targets.end(), event.controlTarget) == targets.end())
            targets.push_back(event.controlTarget);
    }

    targets.erase(std::remove_if(targets.begin(),
                                 targets.end(),
                                 [&processor, engine, safeStripIndex](ScenePerformanceControlTarget target)
                                 {
                                     for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
                                     {
                                         const float depth = engine->getModDepthForSlot(safeStripIndex, slot);
                                         if (depth <= 0.001f)
                                             continue;
                                         if (sceneControlTargetMatchesModTarget(
                                                 processor,
                                                 safeStripIndex,
                                                 target,
                                                 engine->getModTargetForSlot(safeStripIndex, slot)))
                                         {
                                             return false;
                                         }
                                     }
                                     return true;
                                 }),
                  targets.end());
    return targets;
}

int findBestSceneEditorEventIndex(const std::vector<ScenePerformanceEvent>& events,
                                  const ScenePerformanceEvent& target)
{
    int bestIndex = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < static_cast<int>(events.size()); ++i)
    {
        const auto& candidate = events[static_cast<size_t>(i)];
        if (candidate.type != target.type)
            continue;

        double score = std::abs(candidate.timeBeats - target.timeBeats) * 8.0;
        if (candidate.type == ScenePerformanceEventType::Trigger)
        {
            if (candidate.stripIndex != target.stripIndex)
                score += 4.0;
            if (candidate.column != target.column)
                score += 1.0;
            if (candidate.isNoteOn != target.isNoteOn)
                score += 2.0;
        }
        else
        {
            if (candidate.controlMode != target.controlMode)
                score += 4.0;
            if (candidate.controlTarget != target.controlTarget)
                score += 4.0;
            if (candidate.stripIndex != target.stripIndex)
                score += 2.0;
            score += std::abs(static_cast<double>(candidate.value - target.value));
        }

        if (score < bestScore)
        {
            bestScore = score;
            bestIndex = i;
        }
    }

    return bestIndex;
}

double sceneTimeBeatsForX(juce::Rectangle<float> timelineBounds, float x, double lengthBeats)
{
    const float normalizedX = juce::jlimit(0.0f,
                                           1.0f,
                                           (x - timelineBounds.getX()) / juce::jmax(1.0f, timelineBounds.getWidth()));
    const double maxBeat = juce::jmax(0.0, std::nextafter(lengthBeats, 0.0));
    return juce::jlimit(0.0, maxBeat, static_cast<double>(normalizedX) * lengthBeats);
}

int sceneTriggerColumnForY(juce::Rectangle<float> timelineBounds, float y)
{
    const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                  1.0f,
                                                  (y - timelineBounds.getY()) / juce::jmax(1.0f, timelineBounds.getHeight()));
    return juce::jlimit(0, 15, static_cast<int>(std::round(normalizedY * 15.0f)));
}

float sceneDefaultNormalizedValueForLane(int laneIndex)
{
    return sceneAutomationLaneDefinition(laneIndex).defaultNormalizedValue;
}

ScenePerformanceEvent makeDefaultSceneControlEventForLane(int stripIndex,
                                                          int laneIndex,
                                                          double timeBeats,
                                                          float normalizedValue,
                                                          bool drawStepped = false)
{
    ScenePerformanceEvent event;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.stripIndex = sceneResolveAutomationStripIndex(stripIndex, laneIndex);
    event.timeBeats = juce::jmax(0.0, timeBeats);
    event.controlRow = 0;
    event.column = juce::jlimit(0, 15, static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, normalizedValue) * 15.0f)));
    event.controlTarget = sceneAutomationLaneTarget(laneIndex);
    event.controlMode = static_cast<int>(sceneControlModeForTarget(event.controlTarget));

    event.value = denormalizeSceneAutomationValue(event, normalizedValue);
    event.drawStepped = drawStepped;
    return event;
}

double sceneAutomationWriteEndBeat(double lengthBeats)
{
    return juce::jmax(0.0, std::nextafter(juce::jmax(1.0, lengthBeats), 0.0));
}

void appendSceneStripAutomationWriteEvents(MlrVSTAudioProcessor& processor,
                                           int stripIndex,
                                           double lengthBeats,
                                           std::vector<ScenePerformanceEvent>& events)
{
    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const double endBeat = sceneAutomationWriteEndBeat(lengthBeats);

    for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
    {
        if (!sceneAutomationLaneVisible(processor, safeStripIndex, lane))
            continue;

        float normalizedValue = sceneAutomationLaneIsBipolar(lane)
            ? 0.5f
            : sceneDefaultNormalizedValueForLane(lane);
        const auto target = sceneAutomationLaneTarget(lane);
        if (sceneCurrentBaseNormalizedValue(processor, safeStripIndex, target, normalizedValue))
            normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);

        events.push_back(makeDefaultSceneControlEventForLane(safeStripIndex, lane, 0.0, normalizedValue));
        if (endBeat > 0.0)
            events.push_back(makeDefaultSceneControlEventForLane(safeStripIndex, lane, endBeat, normalizedValue));
    }
}

void appendSceneStripStoredDefaultAutomationStartPoints(MlrVSTAudioProcessor& processor,
                                                        int sceneSlot,
                                                        int stripIndex,
                                                        std::vector<ScenePerformanceEvent>& events)
{
    juce::ignoreUnused(sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);

    for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
    {
        if (!sceneAutomationLaneVisible(processor, safeStripIndex, lane))
            continue;

        float normalizedValue = sceneAutomationLaneIsBipolar(lane)
            ? 0.5f
            : sceneDefaultNormalizedValueForLane(lane);

        events.push_back(makeDefaultSceneControlEventForLane(safeStripIndex,
                                                             lane,
                                                             0.0,
                                                             normalizedValue));
    }
}

void appendSceneGlobalAutomationWriteEvents(MlrVSTAudioProcessor& processor,
                                            double lengthBeats,
                                            std::vector<ScenePerformanceEvent>& events)
{
    const int globalLane = sceneGlobalAutomationLaneIndex();
    if (globalLane < 0 || globalLane >= kSceneAutomationLaneCount)
        return;

    const double endBeat = sceneAutomationWriteEndBeat(lengthBeats);
    float normalizedValue = sceneDefaultNormalizedValueForLane(globalLane);
    const auto target = sceneAutomationLaneTarget(globalLane);
    if (sceneCurrentBaseNormalizedValue(processor, -1, target, normalizedValue))
        normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);

    events.push_back(makeDefaultSceneControlEventForLane(-1, globalLane, 0.0, normalizedValue));
    if (endBeat > 0.0)
        events.push_back(makeDefaultSceneControlEventForLane(-1, globalLane, endBeat, normalizedValue));
}

void ensureSceneDefaultAutomationStartPoints(MlrVSTAudioProcessor& processor,
                                             std::vector<ScenePerformanceEvent>& events)
{
    constexpr double kDefaultPointEpsilon = 1.0e-4;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
        {
            if (!sceneAutomationLaneVisible(processor, stripIndex, lane))
                continue;

            const bool hasStartPoint = std::any_of(events.begin(),
                                                   events.end(),
                                                   [stripIndex, lane](const ScenePerformanceEvent& event)
                                                   {
                                                       return event.type == ScenePerformanceEventType::ControlPoint
                                                           && event.stripIndex == stripIndex
                                                           && sceneAutomationLaneIndex(event) == lane
                                                           && std::abs(event.timeBeats) <= kDefaultPointEpsilon;
                                                   });
            if (hasStartPoint)
                continue;

            float normalizedValue = sceneDefaultNormalizedValueForLane(lane);
            const auto target = sceneAutomationLaneTarget(lane);
            if (sceneCurrentBaseNormalizedValue(processor, stripIndex, target, normalizedValue))
                normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);

            events.push_back(makeDefaultSceneControlEventForLane(stripIndex,
                                                                 lane,
                                                                 0.0,
                                                                 normalizedValue));
        }
    }

    const int globalLane = sceneGlobalAutomationLaneIndex();
    if (globalLane >= 0 && globalLane < kSceneAutomationLaneCount)
    {
        const bool hasGlobalStartPoint = std::any_of(events.begin(),
                                                     events.end(),
                                                     [globalLane](const ScenePerformanceEvent& event)
                                                     {
                                                         return sceneIsGlobalAutomationEvent(event)
                                                             && sceneAutomationLaneIndex(event) == globalLane
                                                             && std::abs(event.timeBeats) <= kDefaultPointEpsilon;
                                                     });
        if (!hasGlobalStartPoint)
        {
            float normalizedValue = sceneDefaultNormalizedValueForLane(globalLane);
            const auto target = sceneAutomationLaneTarget(globalLane);
            if (sceneCurrentBaseNormalizedValue(processor, -1, target, normalizedValue))
                normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);

            events.push_back(makeDefaultSceneControlEventForLane(-1,
                                                                 globalLane,
                                                                 0.0,
                                                                 normalizedValue));
        }
    }
}

float sceneStripVerticalScale(bool heightExpanded)
{
    return heightExpanded ? 2.55f : 1.0f;
}

float sceneGlobalLaneCardHeight(bool expanded)
{
    const float laneHeight = expanded ? (kSceneAutomationLaneHeight * kSceneGlobalLaneExpandedScale)
                                      : kSceneAutomationLaneHeight;
    return (kSceneCardPaddingY * 2.0f)
        + kSceneCardHeaderHeight
        + 4.0f
        + laneHeight;
}

float sceneGlobalLaneSectionHeight(bool expanded)
{
    return sceneGlobalLaneCardHeight(expanded) + kSceneCardGap;
}

SceneGlobalLaneLayout makeSceneGlobalLaneLayout(juce::Rectangle<float> cardBounds, bool expanded)
{
    SceneGlobalLaneLayout layout;
    layout.cardBounds = cardBounds;

    auto inner = cardBounds.reduced(kSceneCardPaddingX, kSceneCardPaddingY);
    layout.headerBounds = inner.removeFromTop(kSceneCardHeaderHeight);
    layout.titleBounds = layout.headerBounds.removeFromLeft(78.0f);
    layout.summaryBounds = layout.headerBounds;
    inner.removeFromTop(4.0f);

    const float laneHeight = expanded ? (kSceneAutomationLaneHeight * kSceneGlobalLaneExpandedScale)
                                      : kSceneAutomationLaneHeight;
    auto laneRow = inner.removeFromTop(laneHeight);
    layout.laneLabelBounds = laneRow.removeFromLeft(kSceneTimelineLabelWidth);
    layout.laneBounds = laneRow;
    return layout;
}

float sceneStripCardHeight(const MlrVSTAudioProcessor& processor,
                           int stripIndex,
                           bool automationExpanded,
                           bool heightExpanded)
{
    const bool scenePlaybackAvailable = sceneStripSupportsScenePlayback(processor, stripIndex);
    if (!heightExpanded || !scenePlaybackAvailable)
    {
        return (kSceneCardPaddingY * 2.0f)
            + kSceneCardHeaderHeight
            + 4.0f
            + kSceneOverviewLaneHeight;
    }

    const float verticalScale = sceneStripVerticalScale(heightExpanded);
    const float triggerHeight = sceneTriggerLaneHeight(processor, stripIndex, heightExpanded);
    float height = (kSceneCardPaddingY * 2.0f)
        + kSceneCardHeaderHeight
        + 4.0f
        + kSceneRulerHeight
        + 3.0f
        + triggerHeight
        + 4.0f
        + kSceneAutomationHeaderHeight;

    if (automationExpanded && scenePlaybackAvailable)
    {
        const int visibleLaneCount = juce::jmax(0, sceneAutomationVisibleLaneCount(processor, stripIndex));
        height += 4.0f;
        height += (visibleLaneCount * (kSceneAutomationLaneHeight * verticalScale));
        height += (juce::jmax(0, visibleLaneCount - 1) * kSceneAutomationLaneGap);
    }

    return height;
}

SceneStripCardLayout makeSceneStripCardLayout(const MlrVSTAudioProcessor& processor,
                                              int stripIndex,
                                              juce::Rectangle<float> cardBounds,
                                              bool automationExpanded,
                                              bool heightExpanded)
{
    SceneStripCardLayout layout;
    layout.cardBounds = cardBounds;
    layout.scenePlaybackAvailable = sceneStripSupportsScenePlayback(processor, stripIndex);
    layout.heightExpanded = heightExpanded;
    layout.compactOverview = !layout.scenePlaybackAvailable || !layout.heightExpanded;
    layout.automationExpanded = !layout.compactOverview && automationExpanded && layout.scenePlaybackAvailable;
    const float verticalScale = sceneStripVerticalScale(heightExpanded);
    layout.stepTriggerLane = sceneStripUsesStepTriggerColumns(processor, stripIndex);
    layout.stepTotalSteps = layout.stepTriggerLane ? sceneStepTotalSteps(processor, stripIndex) : 0;
    layout.stepRowCount = layout.stepTriggerLane ? sceneStepRowCount(layout.stepTotalSteps) : 0;

    auto inner = cardBounds.reduced(kSceneCardPaddingX, kSceneCardPaddingY);
    layout.headerBounds = inner.removeFromTop(kSceneCardHeaderHeight);
    layout.titleBounds = layout.headerBounds.removeFromLeft(78.0f);
    auto actionBounds = layout.headerBounds.removeFromRight(204.0f);
    actionBounds = actionBounds.reduced(0.0f, 2.0f);
    layout.stripCopyBounds = actionBounds.removeFromRight(38.0f);
    actionBounds.removeFromRight(4.0f);
    layout.stripDuplicateBounds = actionBounds.removeFromRight(34.0f);
    actionBounds.removeFromRight(4.0f);
    layout.stripClearBounds = actionBounds.removeFromRight(34.0f);
    actionBounds.removeFromRight(4.0f);
    layout.stripWriteAllBounds = actionBounds.removeFromRight(30.0f);
    actionBounds.removeFromRight(4.0f);
    layout.stripWriteBounds = actionBounds.removeFromRight(44.0f);

    layout.summaryBounds = layout.headerBounds;
    inner.removeFromTop(4.0f);

    if (layout.compactOverview)
    {
        auto overviewRow = inner.removeFromTop(kSceneOverviewLaneHeight);
        layout.triggerLabelBounds = overviewRow.removeFromLeft(kSceneTimelineLabelWidth);
        layout.triggerTimelineBounds = overviewRow;
        return layout;
    }

    auto rulerRow = inner.removeFromTop(kSceneRulerHeight);
    layout.rulerLabelBounds = rulerRow.removeFromLeft(kSceneTimelineLabelWidth);
    layout.rulerTimelineBounds = rulerRow;
    inner.removeFromTop(3.0f);

    auto triggerRow = inner.removeFromTop(sceneTriggerLaneHeight(processor, stripIndex, heightExpanded));
    layout.triggerLabelBounds = triggerRow.removeFromLeft(kSceneTimelineLabelWidth);
    layout.triggerTimelineBounds = triggerRow;
    if (layout.stepTriggerLane)
    {
        auto stepContentBounds = layout.triggerTimelineBounds.reduced(1.0f, 1.0f);
        layout.stepLaunchBounds = stepContentBounds.removeFromTop(kSceneStepLaunchHeight * (heightExpanded ? 1.35f : 1.0f));
        stepContentBounds.removeFromTop(kSceneStepPatternGap * (heightExpanded ? 1.25f : 1.0f));
        layout.stepPatternBounds = stepContentBounds;
    }
    inner.removeFromTop(4.0f);

    layout.automationHeaderBounds = inner.removeFromTop(kSceneAutomationHeaderHeight);
    layout.automationToggleBounds = layout.automationHeaderBounds.removeFromLeft(120.0f);

    if (layout.automationExpanded)
    {
        inner.removeFromTop(4.0f);
        for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
        {
            if (!sceneAutomationLaneVisible(processor, stripIndex, lane))
            {
                layout.automationLabelBounds[static_cast<size_t>(lane)] = {};
                layout.automationTimelineBounds[static_cast<size_t>(lane)] = {};
                continue;
            }

            auto row = inner.removeFromTop(kSceneAutomationLaneHeight * verticalScale);
            layout.automationLabelBounds[static_cast<size_t>(lane)] = row.removeFromLeft(kSceneTimelineLabelWidth);
            layout.automationTimelineBounds[static_cast<size_t>(lane)] = row;
            const bool hasMoreVisibleLanes = [&processor, stripIndex, lane]()
            {
                for (int nextLane = lane + 1; nextLane < kSceneAutomationLaneCount; ++nextLane)
                {
                    if (sceneAutomationLaneVisible(processor, stripIndex, nextLane))
                        return true;
                }
                return false;
            }();
            if (hasMoreVisibleLanes)
                inner.removeFromTop(kSceneAutomationLaneGap);
        }

        auto& mutableProcessor = const_cast<MlrVSTAudioProcessor&>(processor);
        for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
        {
            if (!sceneLaneCanShowMotionTargetSelector(mutableProcessor, stripIndex, lane))
                continue;

            const auto labelBounds = layout.automationLabelBounds[static_cast<size_t>(lane)];
            if (labelBounds.isEmpty())
                continue;

            const float comboHeight = juce::jlimit(12.0f, 16.0f, labelBounds.getHeight() - 4.0f);
            layout.motionTargetBounds[static_cast<size_t>(lane)] = juce::Rectangle<float>(
                labelBounds.getX(),
                labelBounds.getBottom() - comboHeight,
                juce::jmax(44.0f, labelBounds.getWidth() - 2.0f),
                comboHeight);
        }
    }

    return layout;
}

juce::Rectangle<float> sceneTriggerMarkerBounds(const SceneStripCardLayout& layout,
                                                const ScenePerformanceEvent& event,
                                                double lengthBeats)
{
    const auto timelineBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
        ? layout.stepLaunchBounds
        : layout.triggerTimelineBounds;
    const float x = timelineBounds.getX()
        + (timelineBounds.getWidth()
           * static_cast<float>(event.timeBeats / juce::jmax(1.0, lengthBeats)));
    if (layout.stepTriggerLane)
    {
        const float markerHeight = juce::jlimit(10.0f, 16.0f, timelineBounds.getHeight() - 4.0f);
        return juce::Rectangle<float>(x - 11.0f,
                                      timelineBounds.getCentreY() - (markerHeight * 0.5f),
                                      22.0f,
                                      markerHeight);
    }

    const float normalizedOffset = juce::jlimit(0.0f, 1.0f, juce::jlimit(0, 15, event.column) / 15.0f);
    const float centerY = timelineBounds.getBottom() - 4.0f
        - (normalizedOffset * juce::jmax(4.0f, timelineBounds.getHeight() - 8.0f));
    return juce::Rectangle<float>(x - 5.5f, centerY - 5.0f, 11.0f, 10.0f);
}

juce::Rectangle<float> sceneTriggerTimeHandleBounds(const SceneStripCardLayout& layout,
                                                    const ScenePerformanceEvent& event,
                                                    double lengthBeats)
{
    if (layout.stepTriggerLane)
        return {};

    const auto marker = sceneTriggerMarkerBounds(layout, event, lengthBeats);
    return juce::Rectangle<float>(marker.getCentreX() - 5.0f,
                                  layout.triggerTimelineBounds.getBottom() - 8.0f,
                                  10.0f,
                                  6.0f);
}

juce::Rectangle<float> sceneTriggerStemBounds(const SceneStripCardLayout& layout,
                                              const ScenePerformanceEvent& event,
                                              double lengthBeats)
{
    if (layout.stepTriggerLane)
        return {};

    const auto marker = sceneTriggerMarkerBounds(layout, event, lengthBeats);
    const float topY = juce::jmin(marker.getCentreY(), layout.triggerTimelineBounds.getBottom() - 8.0f);
    return juce::Rectangle<float>(marker.getCentreX() - 2.5f,
                                  topY,
                                  5.0f,
                                  juce::jmax(6.0f, (layout.triggerTimelineBounds.getBottom() - 5.0f) - topY));
}

juce::Rectangle<float> sceneTriggerInteractiveBounds(const SceneStripCardLayout& layout,
                                                     const ScenePerformanceEvent& event,
                                                     double lengthBeats)
{
    auto marker = sceneTriggerMarkerBounds(layout, event, lengthBeats);
    if (layout.stepTriggerLane)
        return marker;

    return marker
        .getUnion(sceneTriggerTimeHandleBounds(layout, event, lengthBeats))
        .getUnion(sceneTriggerStemBounds(layout, event, lengthBeats));
}

bool sceneResolveTriggerDragIntent(const SceneStripCardLayout& layout,
                                   const ScenePerformanceEvent& event,
                                   double lengthBeats,
                                   juce::Point<float> position,
                                   bool& moveTime,
                                   bool& moveOffset)
{
    if (layout.stepTriggerLane)
    {
        moveTime = true;
        moveOffset = false;
        return sceneTriggerInteractiveBounds(layout, event, lengthBeats).expanded(3.0f, 3.0f).contains(position);
    }

    const auto timeHandleBounds = sceneTriggerTimeHandleBounds(layout, event, lengthBeats);
    const auto offsetHandleBounds = sceneTriggerMarkerBounds(layout, event, lengthBeats);
    const auto stemBounds = sceneTriggerStemBounds(layout, event, lengthBeats);

    moveTime = true;
    moveOffset = true;

    if (timeHandleBounds.expanded(3.0f, 3.0f).contains(position))
    {
        moveOffset = false;
        return true;
    }

    if (offsetHandleBounds.expanded(3.0f, 3.0f).contains(position))
    {
        moveTime = false;
        return true;
    }

    if (stemBounds.expanded(3.0f, 3.0f).contains(position))
        return true;

    if (sceneTriggerInteractiveBounds(layout, event, lengthBeats).expanded(3.0f, 3.0f).contains(position))
        return true;

    moveTime = true;
    moveOffset = true;
    return false;
}

juce::Rectangle<float> sceneControlMarkerBounds(juce::Rectangle<float> laneBounds,
                                                const ScenePerformanceEvent& event,
                                                double lengthBeats)
{
    if (laneBounds.isEmpty())
        return {};

    const float markerRadius = kSceneAutomationMarkerSize * 0.5f;
    const float x = laneBounds.getX()
        + (laneBounds.getWidth() * static_cast<float>(event.timeBeats / juce::jmax(1.0, lengthBeats)));
    const float normalizedValue = normalizeSceneAutomationValue(event);
    const float valueY = laneBounds.getBottom()
        - (normalizedValue * juce::jmax(kSceneAutomationMarkerSize, laneBounds.getHeight() - kSceneAutomationMarkerSize))
        - markerRadius;
    return juce::Rectangle<float>(x - markerRadius,
                                  valueY - markerRadius,
                                  kSceneAutomationMarkerSize,
                                  kSceneAutomationMarkerSize);
}

juce::Rectangle<float> sceneControlMarkerBounds(const SceneStripCardLayout& layout,
                                                const ScenePerformanceEvent& event,
                                                double lengthBeats)
{
    const int laneIndex = sceneAutomationLaneIndex(event);
    if (laneIndex < 0 || laneIndex >= kSceneAutomationLaneCount)
        return {};

    return sceneControlMarkerBounds(layout.automationTimelineBounds[static_cast<size_t>(laneIndex)],
                                    event,
                                    lengthBeats);
}

void drawSceneAutomationPoint(juce::Graphics& g,
                              const ScenePerformanceEvent& event,
                              juce::Rectangle<float> markerBounds,
                              bool selected,
                              juce::Colour colourOverride = {})
{
    const auto colour = colourOverride.isTransparent()
        ? sceneAutomationColour(event).withAlpha(0.95f)
        : colourOverride;
    const float shapeInset = juce::jmax(0.45f, markerBounds.getWidth() * 0.16f);
    const float edgeInset = juce::jmax(0.6f, markerBounds.getWidth() * 0.18f);
    g.setColour(colour);

    switch (juce::jlimit(0, 5, event.controlRow))
    {
        case 1:
            g.fillRoundedRectangle(markerBounds.reduced(shapeInset), juce::jmax(0.7f, markerBounds.getWidth() * 0.18f));
            break;
        case 2:
        {
            juce::Path diamond;
            diamond.addTriangle(markerBounds.getCentreX(), markerBounds.getY() + edgeInset,
                                markerBounds.getRight() - edgeInset, markerBounds.getCentreY(),
                                markerBounds.getCentreX(), markerBounds.getBottom() - edgeInset);
            diamond.addTriangle(markerBounds.getCentreX(), markerBounds.getY() + edgeInset,
                                markerBounds.getX() + edgeInset, markerBounds.getCentreY(),
                                markerBounds.getCentreX(), markerBounds.getBottom() - edgeInset);
            g.fillPath(diamond);
            break;
        }
        case 3:
        {
            juce::Path triangle;
            triangle.addTriangle(markerBounds.getCentreX(), markerBounds.getY() + edgeInset,
                                 markerBounds.getRight() - edgeInset, markerBounds.getBottom() - edgeInset,
                                 markerBounds.getX() + edgeInset, markerBounds.getBottom() - edgeInset);
            g.fillPath(triangle);
            break;
        }
        case 4:
        {
            juce::Path triangle;
            triangle.addTriangle(markerBounds.getX() + edgeInset, markerBounds.getY() + edgeInset,
                                 markerBounds.getRight() - edgeInset, markerBounds.getY() + edgeInset,
                                 markerBounds.getCentreX(), markerBounds.getBottom() - edgeInset);
            g.fillPath(triangle);
            break;
        }
        default:
            g.fillEllipse(markerBounds.reduced(shapeInset));
            break;
    }

    if (selected)
    {
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.drawEllipse(markerBounds.reduced(juce::jmax(0.3f, markerBounds.getWidth() * 0.08f)),
                      juce::jmax(0.85f, markerBounds.getWidth() * 0.18f));
    }
}

void drawSceneHeaderActionChip(juce::Graphics& g,
                               juce::Rectangle<float> bounds,
                               const juce::String& label,
                               juce::Colour colour,
                               bool enabled)
{
    const auto fill = enabled ? colour.withAlpha(0.18f) : juce::Colour(0xff2d3135);
    const auto outline = enabled ? colour.withAlpha(0.46f) : juce::Colours::white.withAlpha(0.05f);
    const auto text = enabled ? colour.brighter(0.25f) : kTextMuted.withAlpha(0.7f);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(outline);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
    g.setColour(text);
    g.setFont(juce::Font(juce::FontOptions(8.6f, juce::Font::bold)));
    g.drawText(label, bounds.toNearestInt(), juce::Justification::centred, false);
}

juce::Colour sceneSlotUiColour(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    return juce::Colour(processor.getSceneInfo(sceneSlot).colourArgb);
}

juce::String sceneLengthModeSummary(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto mode = processor.getSceneLengthMode(sceneSlot);
    const int count = processor.getSceneLengthCount(sceneSlot);
    switch (mode)
    {
        case MlrVSTAudioProcessor::SceneLengthMode::LongestStrip:
            return "Longest x" + juce::String(count);
        case MlrVSTAudioProcessor::SceneLengthMode::LongestPattern:
            return "Pattern x" + juce::String(count);
        case MlrVSTAudioProcessor::SceneLengthMode::ManualBars:
            return juce::String(count) + (count == 1 ? " bar" : " bars");
        case MlrVSTAudioProcessor::SceneLengthMode::AnchorStrip:
            return "Anchor S" + juce::String(processor.getSceneAnchorStrip(sceneSlot) + 1) + " x" + juce::String(count);
        default:
            break;
    }

    return {};
}

juce::String sceneChainTransitionTypeLabel(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    switch (type)
    {
        case TransitionType::Fill:       return "Fill";
        case TransitionType::Stutter:    return "Stutter";
        case TransitionType::FilterRise: return "Rise";
        case TransitionType::Drop:       return "Drop";
        case TransitionType::MuteTail:   return "Mute";
        case TransitionType::Break:      return "Break";
        case TransitionType::Return:     return "Return";
        case TransitionType::None:
        default:
            return "None";
    }
}

juce::String sceneChainTransitionOptionLabel(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    using TransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;
    switch (option)
    {
        case TransitionOption::Snap:    return "Snap";
        case TransitionOption::Tight:   return "Tight";
        case TransitionOption::Wide:    return "Wide";
        case TransitionOption::Wash:    return "Wash";
        case TransitionOption::Echo:    return "Echo";
        case TransitionOption::Sweep:   return "Sweep";
        case TransitionOption::Gate:    return "Gate";
        case TransitionOption::Default:
        default:
            return "Default";
    }
}

juce::String sceneChainTransitionTypeSummary(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    switch (type)
    {
        case TransitionType::Fill:       return "repeat burst";
        case TransitionType::Stutter:    return "retrigger gate";
        case TransitionType::FilterRise: return "filter lift";
        case TransitionType::Drop:       return "drop + fade";
        case TransitionType::MuteTail:   return "tail cut";
        case TransitionType::Break:      return "hard break";
        case TransitionType::Return:     return "auto-return";
        case TransitionType::None:
        default:
            return "straight switch";
    }
}

juce::String sceneChainTransitionOptionSummary(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    using TransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;
    switch (option)
    {
        case TransitionOption::Snap:    return "shortest lead • driest hit";
        case TransitionOption::Tight:   return "short lead • light FX";
        case TransitionOption::Wide:    return "longer lead • bigger movement";
        case TransitionOption::Wash:    return "longest lead • smeared atmosphere";
        case TransitionOption::Echo:    return "delay-heavy tails";
        case TransitionOption::Sweep:   return "filter-heavy sweep";
        case TransitionOption::Gate:    return "hard chops • punchier accents";
        case TransitionOption::Default:
        default:
            return "balanced lead • balanced FX";
    }
}

juce::String sceneChainTransitionScopeLabel(MlrVSTAudioProcessor::SceneChainTransitionScope scope)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    switch (scope)
    {
        case Scope::Loops:  return "Loops";
        case Scope::Steps:  return "Steps";
        case Scope::Grains: return "Grains";
        case Scope::Flip:   return "Flip";
        case Scope::All:
        default:
            return "All";
    }
}

juce::String sceneChainTransitionScopeSummary(MlrVSTAudioProcessor::SceneChainTransitionScope scope)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    switch (scope)
    {
        case Scope::Loops:  return "loop, gate, and one-shot strips";
        case Scope::Steps:  return "step sequencer strips";
        case Scope::Grains: return "granular strips";
        case Scope::Flip:   return "flip/sample strips";
        case Scope::All:
        default:
            return "all playable strips";
    }
}

juce::String sceneChainTransitionContourLabel(MlrVSTAudioProcessor::SceneChainTransitionContour contour)
{
    using Contour = MlrVSTAudioProcessor::SceneChainTransitionContour;
    switch (contour)
    {
        case Contour::Ramp:         return "Ramp";
        case Contour::BurstEnd:     return "Burst";
        case Contour::DuckThenLift: return "Duck/Lift";
        case Contour::LateHit:      return "Late Hit";
        case Contour::Smooth:
        default:
            return "Smooth";
    }
}

juce::String sceneChainTransitionContourSummary(MlrVSTAudioProcessor::SceneChainTransitionContour contour)
{
    using Contour = MlrVSTAudioProcessor::SceneChainTransitionContour;
    switch (contour)
    {
        case Contour::Ramp:         return "steady rise toward the handoff";
        case Contour::BurstEnd:     return "back-loaded burst into the scene change";
        case Contour::DuckThenLift: return "early duck with a later lift";
        case Contour::LateHit:      return "late punch in the final moment";
        case Contour::Smooth:
        default:
            return "even S-curve movement across the whole fill";
    }
}

juce::String sceneChainTransitionConditionLabel(MlrVSTAudioProcessor::SceneChainTransitionCondition condition)
{
    using Condition = MlrVSTAudioProcessor::SceneChainTransitionCondition;
    switch (condition)
    {
        case Condition::Chance50:   return "50%";
        case Condition::Chance25:   return "25%";
        case Condition::LoopOnly:   return "Loop";
        case Condition::ForwardOnly:return "Forward";
        case Condition::Always:
        default:
            return "Always";
    }
}

juce::String sceneChainTransitionConditionSummary(MlrVSTAudioProcessor::SceneChainTransitionCondition condition)
{
    using Condition = MlrVSTAudioProcessor::SceneChainTransitionCondition;
    switch (condition)
    {
        case Condition::Chance50:    return "fires on about half of eligible handoffs";
        case Condition::Chance25:    return "fires on about a quarter of eligible handoffs";
        case Condition::LoopOnly:    return "fires only when the chain wraps back through its loop";
        case Condition::ForwardOnly: return "fires only on forward, non-loop handoffs";
        case Condition::Always:
        default:
            return "fires on every eligible handoff";
    }
}

struct SceneTransitionComboOption
{
    int comboId = 0;
    float value = 0.0f;
    const char* label = "";
};

constexpr std::array<SceneTransitionComboOption, 6> kSceneTransitionLengthOptions{{
    { 1, 0.25f, "1/4b" },
    { 2, 0.5f,  "1/2b" },
    { 3, 1.0f,  "1b" },
    { 4, 2.0f,  "2b" },
    { 5, 4.0f,  "4b" },
    { 6, 8.0f,  "8b" },
}};

template <size_t N>
float sceneTransitionValueForComboId(int comboId,
                                     const std::array<SceneTransitionComboOption, N>& options,
                                     float fallback)
{
    for (const auto& option : options)
        if (option.comboId == comboId)
            return option.value;
    return fallback;
}

template <size_t N>
int sceneTransitionComboIdForValue(float value,
                                   const std::array<SceneTransitionComboOption, N>& options,
                                   float fallback)
{
    float bestDistance = std::numeric_limits<float>::max();
    int bestId = options.front().comboId;
    const float safeValue = std::isfinite(value) ? value : fallback;
    for (const auto& option : options)
    {
        const float distance = std::abs(option.value - safeValue);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestId = option.comboId;
        }
    }
    return bestId;
}

template <size_t N>
int sceneTransitionOptionIndexForComboId(int comboId,
                                         const std::array<SceneTransitionComboOption, N>& options)
{
    for (int index = 0; index < static_cast<int>(N); ++index)
    {
        if (options[static_cast<size_t>(index)].comboId == comboId)
            return index;
    }
    return -1;
}

float steppedSceneTransitionLengthBeats(float value, bool increase)
{
    const int currentComboId = sceneTransitionComboIdForValue(value,
                                                              kSceneTransitionLengthOptions,
                                                              MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats);
    const int currentIndex = sceneTransitionOptionIndexForComboId(currentComboId, kSceneTransitionLengthOptions);
    if (currentIndex < 0)
        return MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;

    const int nextIndex = juce::jlimit(0,
                                       static_cast<int>(kSceneTransitionLengthOptions.size()) - 1,
                                       currentIndex + (increase ? 1 : -1));
    return kSceneTransitionLengthOptions[static_cast<size_t>(nextIndex)].value;
}

float sceneTransitionLengthBeatsForDrag(float startValue, float deltaX)
{
    const int currentComboId = sceneTransitionComboIdForValue(startValue,
                                                              kSceneTransitionLengthOptions,
                                                              MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats);
    const int currentIndex = sceneTransitionOptionIndexForComboId(currentComboId, kSceneTransitionLengthOptions);
    if (currentIndex < 0)
        return MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;

    const int indexOffset = juce::roundToInt(deltaX / 18.0f);
    const int nextIndex = juce::jlimit(0,
                                       static_cast<int>(kSceneTransitionLengthOptions.size()) - 1,
                                       currentIndex + indexOffset);
    return kSceneTransitionLengthOptions[static_cast<size_t>(nextIndex)].value;
}

juce::String formatSceneTransitionLengthBeats(float beats)
{
    const float safeBeats = sceneTransitionValueForComboId(
        sceneTransitionComboIdForValue(beats,
                                       kSceneTransitionLengthOptions,
                                       MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats),
        kSceneTransitionLengthOptions,
        MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats);
    for (const auto& option : kSceneTransitionLengthOptions)
        if (std::abs(option.value - safeBeats) <= 1.0e-4f)
            return option.label;
    return juce::String(safeBeats, 2) + "b";
}

juce::String sceneTransitionParameterSummary(float lengthBeats,
                                             MlrVSTAudioProcessor::SceneChainTransitionScope scope,
                                             MlrVSTAudioProcessor::SceneChainTransitionContour contour,
                                             MlrVSTAudioProcessor::SceneChainTransitionCondition condition,
                                             float intensity,
                                             float delayAmount,
                                             float filterAmount,
                                             float chopAmount)
{
    return formatSceneTransitionLengthBeats(lengthBeats)
        + " • " + sceneChainTransitionScopeLabel(scope)
        + " • " + sceneChainTransitionContourLabel(contour)
        + " • " + sceneChainTransitionConditionLabel(condition)
        + " • Mix " + juce::String(juce::roundToInt(intensity * 100.0f)) + "%"
        + " • Dly " + juce::String(juce::roundToInt(delayAmount * 100.0f)) + "%"
        + " • Flt " + juce::String(juce::roundToInt(filterAmount * 100.0f)) + "%"
        + " • Chop " + juce::String(juce::roundToInt(chopAmount * 100.0f)) + "%";
}

juce::String sceneTransitionTimingModeSummary(bool subtractFromSceneLength)
{
    return subtractFromSceneLength
        ? "Fill shortens the outgoing scene by its lead time"
        : "Fill runs on top of the scene end without shortening it";
}

juce::String sceneTransitionSubtractButtonLabel(bool enabled)
{
    return enabled ? "Shorten Scene" : "Ride Over End";
}

struct SceneTransitionTuningProfile
{
    float defaultLengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    float defaultIntensity = MlrVSTAudioProcessor::DefaultSceneTransitionIntensity;
    float delayWeight = 1.0f;
    float filterWeight = 1.0f;
    float chopWeight = 1.0f;
    bool subtractFromSceneLength = false;
};

SceneTransitionTuningProfile sceneTransitionTypeTuningProfile(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;

    switch (type)
    {
        case TransitionType::Fill:       return { 1.0f, 0.78f, 0.62f, 0.48f, 0.34f, false };
        case TransitionType::Stutter:    return { 0.5f, 0.84f, 0.18f, 0.24f, 1.00f, true };
        case TransitionType::FilterRise: return { 2.0f, 0.80f, 0.18f, 1.00f, 0.12f, false };
        case TransitionType::Drop:       return { 1.0f, 0.86f, 0.34f, 0.76f, 0.58f, true };
        case TransitionType::MuteTail:   return { 0.5f, 0.72f, 0.10f, 0.14f, 0.82f, true };
        case TransitionType::Break:      return { 1.0f, 0.88f, 0.44f, 0.34f, 0.76f, true };
        case TransitionType::Return:     return { 1.0f, 0.64f, 0.20f, 0.26f, 0.16f, false };
        case TransitionType::None:
        default:
            return { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };
    }
}

SceneTransitionTuningProfile sceneTransitionOptionTuningProfile(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    using TransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;

    switch (option)
    {
        case TransitionOption::Snap:    return { 0.5f, 0.92f, 0.48f, 0.72f, 1.14f, true };
        case TransitionOption::Tight:   return { 0.75f, 0.96f, 0.70f, 0.82f, 1.08f, false };
        case TransitionOption::Wide:    return { 1.5f, 1.04f, 1.08f, 1.08f, 0.90f, false };
        case TransitionOption::Wash:    return { 2.0f, 0.94f, 1.34f, 1.20f, 0.66f, false };
        case TransitionOption::Echo:    return { 1.25f, 0.98f, 1.56f, 0.82f, 0.72f, false };
        case TransitionOption::Sweep:   return { 1.25f, 0.98f, 0.84f, 1.58f, 0.76f, false };
        case TransitionOption::Gate:    return { 0.85f, 1.02f, 0.68f, 0.78f, 1.44f, true };
        case TransitionOption::Default:
        default:
            return { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, false };
    }
}

struct SceneTransitionSuggestedSettings
{
    float lengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    float intensity = MlrVSTAudioProcessor::DefaultSceneTransitionIntensity;
    float delayAmount = MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount;
    float filterAmount = MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount;
    float chopAmount = MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount;
    bool subtractFromSceneLength = false;
};

float normalizedSceneTransitionLead(float lengthBeats)
{
    const float safeLength = juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                                          MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                                          std::isfinite(lengthBeats)
                                              ? lengthBeats
                                              : MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats);
    return juce::jlimit(0.0f,
                        1.0f,
                        (safeLength - MlrVSTAudioProcessor::MinSceneTransitionLengthBeats)
                            / (MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats
                               - MlrVSTAudioProcessor::MinSceneTransitionLengthBeats));
}

SceneTransitionSuggestedSettings sceneTransitionSuggestedSettings(MlrVSTAudioProcessor::SceneChainTransitionType type,
                                                                 MlrVSTAudioProcessor::SceneChainTransitionOption option,
                                                                 float intensity,
                                                                 float lengthBeats,
                                                                 bool subtractFromSceneLength)
{
    SceneTransitionSuggestedSettings settings;
    const auto typeProfile = sceneTransitionTypeTuningProfile(type);
    const auto optionProfile = sceneTransitionOptionTuningProfile(option);

    if (type == MlrVSTAudioProcessor::SceneChainTransitionType::None)
    {
        settings.intensity = 0.0f;
        settings.delayAmount = 0.0f;
        settings.filterAmount = 0.0f;
        settings.chopAmount = 0.0f;
        settings.subtractFromSceneLength = false;
        return settings;
    }

    settings.lengthBeats = juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                                        MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                                        std::isfinite(lengthBeats)
                                            ? lengthBeats
                                            : (typeProfile.defaultLengthBeats * optionProfile.defaultLengthBeats));
    settings.intensity = juce::jlimit(0.0f,
                                      1.0f,
                                      std::isfinite(intensity)
                                          ? intensity
                                          : (typeProfile.defaultIntensity * optionProfile.defaultIntensity));

    const float leadNormal = normalizedSceneTransitionLead(settings.lengthBeats);
    const float shortLeadFocus = 1.0f - leadNormal;
    const float longLeadFocus = leadNormal;

    const float delayWeight = juce::jmax(0.0f,
                                         typeProfile.delayWeight
                                             * optionProfile.delayWeight
                                             * (0.78f + (0.52f * longLeadFocus)));
    const float filterWeight = juce::jmax(0.0f,
                                          typeProfile.filterWeight
                                              * optionProfile.filterWeight
                                              * (0.80f + (0.54f * longLeadFocus)));
    const float chopWeight = juce::jmax(0.0f,
                                        typeProfile.chopWeight
                                            * optionProfile.chopWeight
                                            * (0.78f + (0.56f * shortLeadFocus)));
    const float maxWeight = juce::jmax(0.0001f, juce::jmax(delayWeight, juce::jmax(filterWeight, chopWeight)));

    auto weightedAmount = [&](float rawWeight, float baseFloor, float range)
    {
        const float normalizedWeight = juce::jlimit(0.0f, 1.0f, rawWeight / maxWeight);
        return juce::jlimit(0.0f, 1.0f, baseFloor + (settings.intensity * range * normalizedWeight));
    };

    settings.delayAmount = weightedAmount(delayWeight, 0.04f + (0.06f * longLeadFocus), 0.78f);
    settings.filterAmount = weightedAmount(filterWeight, 0.05f + (0.05f * longLeadFocus), 0.80f);
    settings.chopAmount = weightedAmount(chopWeight, 0.04f + (0.06f * shortLeadFocus), 0.76f);
    settings.subtractFromSceneLength = subtractFromSceneLength
        || typeProfile.subtractFromSceneLength
        || optionProfile.subtractFromSceneLength;
    return settings;
}

juce::String sceneTransitionFocusSummary(float delayAmount, float filterAmount, float chopAmount)
{
    const float delay = juce::jlimit(0.0f, 1.0f, delayAmount);
    const float filter = juce::jlimit(0.0f, 1.0f, filterAmount);
    const float chop = juce::jlimit(0.0f, 1.0f, chopAmount);

    if (juce::jmax(delay, juce::jmax(filter, chop)) < 0.18f)
        return "subtle";
    if (delay >= filter + 0.10f && delay >= chop + 0.10f)
        return "echo-led";
    if (filter >= delay + 0.10f && filter >= chop + 0.10f)
        return "sweep-led";
    if (chop >= delay + 0.10f && chop >= filter + 0.10f)
        return "gate-led";
    if (delay > 0.46f && filter > 0.46f)
        return "wide lift";
    if (filter > 0.42f && chop > 0.42f)
        return "cutting break";
    return "balanced";
}

juce::String sceneTransitionEnergySummary(float intensity)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, intensity);
    if (clamped < 0.22f)
        return "very light";
    if (clamped < 0.46f)
        return "light";
    if (clamped < 0.74f)
        return "driving";
    if (clamped < 0.92f)
        return "high energy";
    return "full send";
}

struct SceneChainLayout
{
    juce::Rectangle<float> bounds;
    juce::Rectangle<float> headerBounds;
    juce::Rectangle<float> legendBounds;
    juce::Rectangle<float> railBounds;
    std::array<juce::Rectangle<float>, MlrVSTAudioProcessor::MaxSceneChainSteps> stepBounds{};
    std::array<juce::Rectangle<float>, MlrVSTAudioProcessor::MaxSceneChainSteps - 1> transitionBounds{};
};

SceneChainLayout makeSceneChainLayout(juce::Rectangle<float> bounds)
{
    SceneChainLayout layout;
    layout.bounds = bounds;
    auto content = bounds.reduced(8.0f, 6.0f);
    layout.headerBounds = content.removeFromTop(0.0f);
    layout.legendBounds = layout.headerBounds;
    layout.railBounds = content;
    const float chipHeight = 13.0f;
    const float stepHeight = juce::jlimit(30.0f, 34.0f, layout.railBounds.getHeight() - 20.0f);
    const float stepTop = layout.railBounds.getY()
        + ((layout.railBounds.getHeight() - stepHeight) * 0.5f);
    const float chipTop = stepTop + ((stepHeight - chipHeight) * 0.5f);

    const float chipWidth = 32.0f;
    const float totalChipWidth = chipWidth * static_cast<float>(MlrVSTAudioProcessor::MaxSceneChainSteps - 1);
    const float stepWidth = juce::jmax(28.0f,
                                       (layout.railBounds.getWidth() - totalChipWidth)
                                           / static_cast<float>(MlrVSTAudioProcessor::MaxSceneChainSteps));
    auto rail = layout.railBounds;
    for (int step = 0; step < MlrVSTAudioProcessor::MaxSceneChainSteps; ++step)
    {
        layout.stepBounds[static_cast<size_t>(step)] =
            juce::Rectangle<float>(rail.getX(), stepTop, stepWidth, stepHeight);
        rail.setX(layout.stepBounds[static_cast<size_t>(step)].getRight());
        if (step < MlrVSTAudioProcessor::MaxSceneChainSteps - 1)
        {
            layout.transitionBounds[static_cast<size_t>(step)] = juce::Rectangle<float>(
                rail.getX(),
                chipTop,
                chipWidth,
                chipHeight);
            rail.setX(layout.transitionBounds[static_cast<size_t>(step)].getRight());
        }
    }

    return layout;
}

juce::Rectangle<float> sceneChainLoopClampBounds(const SceneChainLayout& layout,
                                                 int loopStart,
                                                 int loopEnd)
{
    if (loopStart < 0
        || loopEnd < loopStart
        || loopEnd >= MlrVSTAudioProcessor::MaxSceneChainSteps)
    {
        return {};
    }

    const auto loopLeft = layout.stepBounds[static_cast<size_t>(loopStart)].getX() + 4.0f;
    const auto loopRight = layout.stepBounds[static_cast<size_t>(loopEnd)].getRight() - 4.0f;
    const float loopTop = juce::jmax(layout.railBounds.getY() + 1.0f,
                                     layout.stepBounds[static_cast<size_t>(loopStart)].getY() - 13.0f);
    return juce::Rectangle<float>(loopLeft - 6.0f,
                                  loopTop,
                                  juce::jmax(14.0f, loopRight - loopLeft + 12.0f),
                                  6.0f);
}

juce::String sceneChainTransitionChipLabel(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    switch (type)
    {
        case TransitionType::Fill:       return "Fill";
        case TransitionType::Stutter:    return "Stut";
        case TransitionType::FilterRise: return "Rise";
        case TransitionType::Drop:       return "Drop";
        case TransitionType::MuteTail:   return "Mute";
        case TransitionType::Break:      return "Break";
        case TransitionType::Return:     return "Back";
        case TransitionType::None:
        default:
            return "None";
    }
}

juce::Colour sceneChainTransitionColour(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    switch (type)
    {
        case TransitionType::Fill:       return juce::Colour(0xffe0b248);
        case TransitionType::Stutter:    return juce::Colour(0xfff06f45);
        case TransitionType::FilterRise: return juce::Colour(0xff58c96d);
        case TransitionType::Drop:       return juce::Colour(0xffe35576);
        case TransitionType::MuteTail:   return juce::Colour(0xff8ca1b8);
        case TransitionType::Break:      return juce::Colour(0xff41b7e8);
        case TransitionType::Return:     return juce::Colour(0xffc48d44);
        case TransitionType::None:
        default:
            return juce::Colour(0xff465158);
    }
}

juce::Colour sceneChainTransitionSecondaryColour(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    switch (type)
    {
        case TransitionType::Fill:       return juce::Colour(0xff6e4f1d);
        case TransitionType::Stutter:    return juce::Colour(0xff6d2c1f);
        case TransitionType::FilterRise: return juce::Colour(0xff1f5730);
        case TransitionType::Drop:       return juce::Colour(0xff5b1f34);
        case TransitionType::MuteTail:   return juce::Colour(0xff334252);
        case TransitionType::Break:      return juce::Colour(0xff173d52);
        case TransitionType::Return:     return juce::Colour(0xff664018);
        case TransitionType::None:
        default:
            return juce::Colour(0xff20262b);
    }
}

MlrVSTAudioProcessor::SceneChainTransitionType nextSceneChainTransitionType(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    const int current = static_cast<int>(type);
    const int maxValue = static_cast<int>(TransitionType::Return);
    return static_cast<TransitionType>((current >= maxValue) ? 0 : (current + 1));
}

MlrVSTAudioProcessor::SceneChainTransitionType previousSceneChainTransitionType(MlrVSTAudioProcessor::SceneChainTransitionType type)
{
    using TransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
    const int current = static_cast<int>(type);
    const int maxValue = static_cast<int>(TransitionType::Return);
    return static_cast<TransitionType>((current <= 0) ? maxValue : (current - 1));
}

MlrVSTAudioProcessor::SceneChainTransitionOption nextSceneChainTransitionOption(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    using TransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;
    const int current = static_cast<int>(option);
    const int maxValue = static_cast<int>(TransitionOption::Gate);
    return static_cast<TransitionOption>((current >= maxValue) ? 0 : (current + 1));
}

MlrVSTAudioProcessor::SceneChainTransitionOption previousSceneChainTransitionOption(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    using TransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;
    const int current = static_cast<int>(option);
    const int maxValue = static_cast<int>(TransitionOption::Gate);
    return static_cast<TransitionOption>((current <= 0) ? maxValue : (current - 1));
}

MlrVSTAudioProcessor::SceneChainTransitionScope nextSceneChainTransitionScope(MlrVSTAudioProcessor::SceneChainTransitionScope scope)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    const int current = static_cast<int>(scope);
    const int maxValue = static_cast<int>(Scope::Flip);
    return static_cast<Scope>((current >= maxValue) ? 0 : (current + 1));
}

MlrVSTAudioProcessor::SceneChainTransitionScope previousSceneChainTransitionScope(MlrVSTAudioProcessor::SceneChainTransitionScope scope)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    const int current = static_cast<int>(scope);
    const int maxValue = static_cast<int>(Scope::Flip);
    return static_cast<Scope>((current <= 0) ? maxValue : (current - 1));
}

MlrVSTAudioProcessor::SceneChainTransitionContour nextSceneChainTransitionContour(MlrVSTAudioProcessor::SceneChainTransitionContour contour)
{
    using Contour = MlrVSTAudioProcessor::SceneChainTransitionContour;
    const int current = static_cast<int>(contour);
    const int maxValue = static_cast<int>(Contour::LateHit);
    return static_cast<Contour>((current >= maxValue) ? 0 : (current + 1));
}

MlrVSTAudioProcessor::SceneChainTransitionContour previousSceneChainTransitionContour(MlrVSTAudioProcessor::SceneChainTransitionContour contour)
{
    using Contour = MlrVSTAudioProcessor::SceneChainTransitionContour;
    const int current = static_cast<int>(contour);
    const int maxValue = static_cast<int>(Contour::LateHit);
    return static_cast<Contour>((current <= 0) ? maxValue : (current - 1));
}

MlrVSTAudioProcessor::SceneChainTransitionCondition nextSceneChainTransitionCondition(MlrVSTAudioProcessor::SceneChainTransitionCondition condition)
{
    using Condition = MlrVSTAudioProcessor::SceneChainTransitionCondition;
    const int current = static_cast<int>(condition);
    const int maxValue = static_cast<int>(Condition::ForwardOnly);
    return static_cast<Condition>((current >= maxValue) ? 0 : (current + 1));
}

MlrVSTAudioProcessor::SceneChainTransitionCondition previousSceneChainTransitionCondition(MlrVSTAudioProcessor::SceneChainTransitionCondition condition)
{
    using Condition = MlrVSTAudioProcessor::SceneChainTransitionCondition;
    const int current = static_cast<int>(condition);
    const int maxValue = static_cast<int>(Condition::ForwardOnly);
    return static_cast<Condition>((current <= 0) ? maxValue : (current - 1));
}

std::vector<MlrVSTAudioProcessor::SceneChainStep> snapshotSceneChainSteps(const MlrVSTAudioProcessor& processor)
{
    std::vector<MlrVSTAudioProcessor::SceneChainStep> steps;
    const int chainLength = processor.getSceneChainLength();
    steps.reserve(static_cast<size_t>(chainLength));
    for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
    {
        MlrVSTAudioProcessor::SceneChainStep step;
        step.sceneSlot = processor.getSceneChainStepSceneSlot(stepIndex);
        step.repeats = processor.getSceneChainStepRepeatCount(stepIndex);
        step.transitionToNext = processor.getSceneChainStepTransitionType(stepIndex);
        step.transitionOption = processor.getSceneChainStepTransitionOption(stepIndex);
        step.transitionScope = processor.getSceneChainStepTransitionScope(stepIndex);
        step.transitionContour = processor.getSceneChainStepTransitionContour(stepIndex);
        step.transitionCondition = processor.getSceneChainStepTransitionCondition(stepIndex);
        step.transitionLengthBeats = processor.getSceneChainStepTransitionLengthBeats(stepIndex);
        step.transitionSubtractsFromSceneLength =
            processor.getSceneChainStepTransitionSubtractsFromSceneLength(stepIndex);
        step.transitionIntensity = processor.getSceneChainStepTransitionIntensity(stepIndex);
        step.transitionDelayAmount = processor.getSceneChainStepTransitionDelayAmount(stepIndex);
        step.transitionFilterAmount = processor.getSceneChainStepTransitionFilterAmount(stepIndex);
        step.transitionChopAmount = processor.getSceneChainStepTransitionChopAmount(stepIndex);
        step.transitionEndSampleFile = processor.getSceneChainStepTransitionEndSampleFile(stepIndex);
        step.transitionEndSampleSettings = processor.getSceneChainStepTransitionEndSampleSettings(stepIndex);
        if (step.sceneSlot >= 0)
            steps.push_back(step);
    }
    return steps;
}

void applySceneChainSteps(MlrVSTAudioProcessor& processor,
                          const std::vector<MlrVSTAudioProcessor::SceneChainStep>& steps,
                          bool loopEnabled,
                          int loopStart,
                          int loopEnd)
{
    processor.clearSceneChain();
    const int safeCount = juce::jmin(static_cast<int>(steps.size()), MlrVSTAudioProcessor::MaxSceneChainSteps);
    for (int stepIndex = 0; stepIndex < safeCount; ++stepIndex)
    {
        const auto& step = steps[static_cast<size_t>(stepIndex)];
        processor.setSceneChainStep(stepIndex, step.sceneSlot, step.repeats);
        processor.setSceneChainStepTransitionType(stepIndex, step.transitionToNext);
        processor.setSceneChainStepTransitionOption(stepIndex, step.transitionOption);
        processor.setSceneChainStepTransitionScope(stepIndex, step.transitionScope);
        processor.setSceneChainStepTransitionContour(stepIndex, step.transitionContour);
        processor.setSceneChainStepTransitionCondition(stepIndex, step.transitionCondition);
        processor.setSceneChainStepTransitionLengthBeats(stepIndex, step.transitionLengthBeats);
        processor.setSceneChainStepTransitionSubtractsFromSceneLength(stepIndex,
                                                                      step.transitionSubtractsFromSceneLength);
        processor.setSceneChainStepTransitionIntensity(stepIndex, step.transitionIntensity);
        processor.setSceneChainStepTransitionDelayAmount(stepIndex, step.transitionDelayAmount);
        processor.setSceneChainStepTransitionFilterAmount(stepIndex, step.transitionFilterAmount);
        processor.setSceneChainStepTransitionChopAmount(stepIndex, step.transitionChopAmount);
        processor.setSceneChainStepTransitionEndSampleFile(stepIndex, step.transitionEndSampleFile);
        processor.setSceneChainStepTransitionEndSampleSettings(stepIndex, step.transitionEndSampleSettings);
    }

    if (loopEnabled && safeCount >= 2)
    {
        const int safeLoopStart = juce::jlimit(0, safeCount - 1, loopStart);
        const int safeLoopEnd = juce::jlimit(safeLoopStart, safeCount - 1, loopEnd);
        processor.setSceneChainLoopRange(safeLoopStart, safeLoopEnd);
        processor.setSceneChainLoopEnabled(true);
    }
}

int sceneChainStepAtPosition(const SceneChainLayout& layout, juce::Point<float> position)
{
    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps; ++stepIndex)
    {
        if (layout.stepBounds[static_cast<size_t>(stepIndex)].contains(position))
            return stepIndex;
    }

    return -1;
}

int sceneChainInsertStepAtPosition(const SceneChainLayout& layout, juce::Point<float> position, int chainLength)
{
    if (!layout.railBounds.expanded(0.0f, 12.0f).contains(position))
        return -1;

    const float x = juce::jlimit(layout.railBounds.getX(), layout.railBounds.getRight(), position.x);
    int insertStep = 0;
    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps; ++stepIndex)
    {
        const auto stepBounds = layout.stepBounds[static_cast<size_t>(stepIndex)];
        if (x <= stepBounds.getCentreX())
        {
            insertStep = stepIndex;
            break;
        }

        insertStep = stepIndex + 1;
    }

    return juce::jlimit(0, juce::jmin(chainLength, MlrVSTAudioProcessor::MaxSceneChainSteps), insertStep);
}

float sceneChainInsertionMarkerX(const SceneChainLayout& layout, int insertStep)
{
    const int safeInsertStep = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps, insertStep);
    if (safeInsertStep <= 0)
        return layout.stepBounds[0].getX();
    if (safeInsertStep >= MlrVSTAudioProcessor::MaxSceneChainSteps)
        return layout.stepBounds[static_cast<size_t>(MlrVSTAudioProcessor::MaxSceneChainSteps - 1)].getRight();
    return layout.stepBounds[static_cast<size_t>(safeInsertStep)].getX();
}

juce::Rectangle<float> sceneChainTransitionHitBounds(const SceneChainLayout& layout, int transitionIndex)
{
    if (transitionIndex < 0 || transitionIndex >= MlrVSTAudioProcessor::MaxSceneChainSteps - 1)
        return {};

    const auto from = layout.stepBounds[static_cast<size_t>(transitionIndex)];
    const auto to = layout.stepBounds[static_cast<size_t>(transitionIndex + 1)];
    const auto chipBounds = layout.transitionBounds[static_cast<size_t>(transitionIndex)];
    const float centerY = chipBounds.getCentreY();
    const float laneTop = centerY - 10.0f;
    const float laneBottom = centerY + 10.0f;
    return juce::Rectangle<float>(from.getRight() + 1.0f,
                                  laneTop,
                                  juce::jmax(1.0f, to.getX() - from.getRight() - 2.0f),
                                  laneBottom - laneTop);
}

int sceneChainTransitionAtPosition(const SceneChainLayout& layout, juce::Point<float> position)
{
    for (int transitionIndex = 0; transitionIndex < MlrVSTAudioProcessor::MaxSceneChainSteps - 1; ++transitionIndex)
    {
        if (sceneChainTransitionHitBounds(layout, transitionIndex).contains(position))
            return transitionIndex;
    }

    return -1;
}
} // namespace

void SceneTimelineCanvas::paint(juce::Graphics& g)
{
    owner.paintSceneTimelineCanvas(g);
}

void SceneTimelineCanvas::mouseDown(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseDown(e);
}

void SceneTimelineCanvas::mouseDoubleClick(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseDoubleClick(e);
}

void SceneTimelineCanvas::mouseDrag(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseDrag(e);
}

void SceneTimelineCanvas::mouseMove(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseMove(e);
}

void SceneTimelineCanvas::mouseExit(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseExit(e);
}

void SceneTimelineCanvas::mouseUp(const juce::MouseEvent& e)
{
    owner.handleSceneTimelineMouseUp(e);
}

void SceneTimelineCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    owner.handleSceneTimelineMouseWheel(e, wheel);
}

void SceneChainCanvas::paint(juce::Graphics& g)
{
    owner.paintSceneChainCanvas(g);
}

void SceneChainCanvas::mouseDown(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseDown(e);
}

void SceneChainCanvas::mouseDoubleClick(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseDoubleClick(e);
}

void SceneChainCanvas::mouseDrag(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseDrag(e);
}

void SceneChainCanvas::mouseMove(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseMove(e);
}

void SceneChainCanvas::mouseExit(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseExit(e);
}

void SceneChainCanvas::mouseUp(const juce::MouseEvent& e)
{
    owner.handleSceneChainMouseUp(e);
}

void SceneChainCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    owner.handleSceneChainMouseWheel(e, wheel);
}

juce::Font SceneControlPanel::SceneControlLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return juce::Font(juce::FontOptions(10.2f, juce::Font::bold));
}

juce::Font SceneControlPanel::SceneControlLookAndFeel::getPopupMenuFont()
{
    return juce::Font(juce::FontOptions(13.5f, juce::Font::bold));
}

juce::Font SceneControlPanel::SceneControlLookAndFeel::getTextButtonFont(juce::TextButton& button, int buttonHeight)
{
    const auto role = button.getProperties()["sceneRole"].toString();
    float size = buttonHeight >= 24 ? 10.8f : 9.8f;
    if (role == "chip")
        size = 9.2f;
    else if (role == "tool")
        size = 9.6f;
    else if (role == "transport")
        size = juce::jmax(11.2f, buttonHeight * 0.46f);
    else if (role == "slot")
        size = juce::jmax(10.4f, buttonHeight * 0.41f);

    return juce::Font(juce::FontOptions(size, juce::Font::bold));
}

void SceneControlPanel::SceneControlLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                                                      juce::Button& button,
                                                                      const juce::Colour& backgroundColour,
                                                                      bool shouldDrawButtonAsHighlighted,
                                                                      bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    if (bounds.isEmpty())
        return;

    const auto role = button.getProperties()["sceneRole"].toString();
    const bool isSlot = role == "slot";
    const bool isChip = role == "chip";
    const bool isTransport = role == "transport";
    const bool slotHasStoredData = !isSlot
        || static_cast<bool>(button.getProperties().getWithDefault("sceneHasStoredData", true));
    const float cornerSize = isSlot ? 8.4f : (isTransport ? 8.6f : (isChip ? 6.0f : 7.0f));

    auto baseColour = button.getToggleState()
        ? button.findColour(juce::TextButton::buttonOnColourId)
        : backgroundColour;

    if (shouldDrawButtonAsDown)
        baseColour = baseColour.brighter(0.10f);
    else if (shouldDrawButtonAsHighlighted)
        baseColour = baseColour.brighter(isSlot ? 0.07f : 0.05f);

    if (isSlot && !slotHasStoredData)
        baseColour = baseColour.interpolatedWith(juce::Colour(0xff3b4249), 0.56f);

    if (!button.isEnabled())
        baseColour = baseColour.withMultipliedAlpha(0.42f);

    g.setColour(juce::Colours::black.withAlpha(isTransport ? 0.28f : 0.20f));
    g.fillRoundedRectangle(bounds.translated(0.0f, isTransport ? 1.4f : 1.0f), cornerSize);

    juce::ColourGradient fill(baseColour.brighter(isTransport ? 0.16f : (isSlot ? 0.10f : 0.05f)),
                              bounds.getX(),
                              bounds.getY(),
                              baseColour.darker(isTransport ? 0.22f : (isChip ? 0.08f : 0.15f)),
                              bounds.getX(),
                              bounds.getBottom(),
                              false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(bounds, cornerSize);

    g.setColour(juce::Colours::white.withAlpha(shouldDrawButtonAsHighlighted
                                                   ? (isTransport ? 0.16f : 0.11f)
                                                   : (isTransport ? 0.10f : 0.06f)));
    g.drawRoundedRectangle(bounds.reduced(0.45f), cornerSize, 1.0f);

    g.setColour(baseColour.contrasting(isTransport ? 0.22f : 0.16f)
                    .withAlpha(isSlot ? 0.28f : (isTransport ? 0.24f : 0.18f)));
    g.drawRoundedRectangle(bounds.reduced(1.35f), juce::jmax(2.0f, cornerSize - 1.4f), 1.0f);

    if (isTransport)
    {
        g.setColour(baseColour.brighter(0.55f).withAlpha(button.getToggleState() ? 0.38f : 0.22f));
        g.drawRoundedRectangle(bounds.reduced(2.2f), juce::jmax(3.0f, cornerSize - 2.2f), 1.2f);
    }
}

void SceneControlPanel::SceneControlLookAndFeel::drawButtonText(juce::Graphics& g,
                                                                juce::TextButton& button,
                                                                bool,
                                                                bool)
{
    const auto role = button.getProperties()["sceneRole"].toString();
    const bool isSlot = role == "slot";
    const bool slotHasStoredData = !isSlot
        || static_cast<bool>(button.getProperties().getWithDefault("sceneHasStoredData", true));
    auto colour = button.findColour(button.getToggleState()
                                        ? juce::TextButton::textColourOnId
                                        : juce::TextButton::textColourOffId);
    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(0.48f);

    auto textBounds = button.getLocalBounds().reduced(8, 0);
    if (isSlot && !slotHasStoredData)
    {
        auto mainTextBounds = textBounds;
        auto badgeBounds = mainTextBounds.removeFromBottom(9);
        mainTextBounds.removeFromBottom(1);

        g.setFont(getTextButtonFont(button, button.getHeight()));
        g.setColour(colour);
        g.drawFittedText(button.getButtonText(),
                         mainTextBounds,
                         juce::Justification::centred,
                         1);

        const auto badgeText = button.getProperties().getWithDefault("sceneDataBadgeText", "EMPTY").toString();
        if (badgeText.isNotEmpty())
        {
            const auto badgeFont = juce::Font(juce::FontOptions(6.0f, juce::Font::bold));
            const int badgeWidth = juce::jlimit(22,
                                                juce::jmax(22, badgeBounds.getWidth()),
                                                juce::GlyphArrangement::getStringWidthInt(badgeFont, badgeText) + 10);
            auto badgeArea = juce::Rectangle<int>(badgeWidth, 7).withCentre(badgeBounds.getCentre());
            badgeArea.setY(badgeBounds.getY() + 1);

            g.setColour(juce::Colour(0xff0f1317).withAlpha(0.70f));
            g.fillRoundedRectangle(badgeArea.toFloat(), 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.11f));
            g.drawRoundedRectangle(badgeArea.toFloat().reduced(0.5f), 3.0f, 0.8f);
            g.setFont(badgeFont);
            g.setColour(colour.withMultipliedAlpha(0.74f));
            g.drawFittedText(badgeText,
                             badgeArea,
                             juce::Justification::centred,
                             1);
        }

        return;
    }

    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(colour);
    g.drawFittedText(button.getButtonText(),
                     textBounds,
                     juce::Justification::centred,
                     1);
}

void SceneControlPanel::SceneControlLookAndFeel::drawComboBox(juce::Graphics& g,
                                                              int width,
                                                              int height,
                                                              bool,
                                                              int,
                                                              int,
                                                              int,
                                                              int,
                                                              juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    if (bounds.isEmpty())
        return;

    auto background = box.findColour(juce::ComboBox::backgroundColourId);
    auto outline = box.findColour(juce::ComboBox::outlineColourId);
    auto arrow = box.findColour(juce::ComboBox::arrowColourId);
    if (!box.isEnabled())
    {
        background = background.withMultipliedAlpha(0.46f);
        outline = outline.withMultipliedAlpha(0.38f);
        arrow = arrow.withMultipliedAlpha(0.40f);
    }

    juce::ColourGradient fill(background.brighter(0.05f),
                              bounds.getX(),
                              bounds.getY(),
                              background.darker(0.12f),
                              bounds.getX(),
                              bounds.getBottom(),
                              false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(bounds, 7.0f);

    const auto sheenBounds = bounds.reduced(1.0f).removeFromTop(bounds.getHeight() * 0.46f);
    g.setColour(juce::Colours::white.withAlpha(0.035f));
    g.fillRoundedRectangle(sheenBounds, 6.0f);

    g.setColour(outline.withAlpha(0.78f));
    g.drawRoundedRectangle(bounds, 7.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawRoundedRectangle(bounds.reduced(1.2f), 5.8f, 1.0f);

    auto arrowBounds = bounds.toNearestInt().removeFromRight(18).reduced(4, 5).toFloat();
    juce::Path arrowPath;
    arrowPath.startNewSubPath(arrowBounds.getX(), arrowBounds.getY() + 1.0f);
    arrowPath.lineTo(arrowBounds.getCentreX(), arrowBounds.getBottom() - 1.0f);
    arrowPath.lineTo(arrowBounds.getRight(), arrowBounds.getY() + 1.0f);
    g.setColour(arrow);
    g.strokePath(arrowPath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void SceneControlPanel::SceneControlLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(10, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(juce::Justification::centredLeft);
}

namespace
{
bool isSceneTransitionEndSampleAudioFile(const juce::File& file)
{
    return file.existsAsFile()
        && file.hasFileExtension(".wav;.aif;.aiff;.mp3;.ogg;.flac");
}

std::vector<juce::File> collectSceneTransitionEndSampleChoices(const juce::File& directory)
{
    std::vector<juce::File> files;
    if (directory == juce::File() || !directory.exists() || !directory.isDirectory())
        return files;

    juce::Array<juce::File> audioFiles;
    for (auto& file : directory.findChildFiles(juce::File::findFiles, false))
    {
        if (isSceneTransitionEndSampleAudioFile(file))
            audioFiles.add(file);
    }

    if (audioFiles.isEmpty())
    {
        for (auto& file : directory.findChildFiles(juce::File::findFiles, true))
        {
            if (isSceneTransitionEndSampleAudioFile(file))
                audioFiles.add(file);
        }
    }

    audioFiles.sort();
    files.reserve(static_cast<size_t>(audioFiles.size()));
    for (const auto& file : audioFiles)
        files.push_back(file);
    return files;
}

juce::File effectiveSceneTransitionEndSampleDirectory(MlrVSTAudioProcessor& processor, int stepIndex)
{
    auto directory = processor.getSceneTransitionEndSampleDirectory();
    if (directory.exists() && directory.isDirectory())
        return directory;

    if (stepIndex >= 0)
    {
        const auto pickedFile = processor.getSceneChainStepTransitionEndSampleFile(stepIndex);
        const auto pickedDirectory = pickedFile.getParentDirectory();
        if (pickedDirectory.exists() && pickedDirectory.isDirectory())
            return pickedDirectory;
    }

    const int chainLength = processor.getSceneChainLength();
    for (int index = 0; index < chainLength; ++index)
    {
        const auto pickedDirectory = processor.getSceneChainStepTransitionEndSampleFile(index).getParentDirectory();
        if (pickedDirectory.exists() && pickedDirectory.isDirectory())
            return pickedDirectory;
    }

    return {};
}

bool sceneTransitionEndSampleDirectoryAvailable(const juce::File& directory)
{
    return directory.exists() && directory.isDirectory();
}

juce::String sceneTransitionEndSampleChoiceLabel(const juce::File& file, const juce::File& directory)
{
    if (file == juce::File())
        return {};

    juce::String label = directory.exists() && directory.isDirectory()
        ? file.getRelativePathFrom(directory)
        : file.getFileName();
    if (label.isEmpty())
        label = file.getFileName();
    return label;
}

juce::String sceneTransitionEndSamplePickedLabel(const juce::File& file)
{
    if (file == juce::File() || file.getFullPathName().trim().isEmpty())
        return "Picked: None";

    if (!file.existsAsFile())
        return "Picked: (Missing) " + file.getFileName();

    return "Picked: " + file.getFileName();
}

bool sceneTransitionEndSampleFilesMatch(const juce::File& lhs, const juce::File& rhs)
{
    return lhs.getFullPathName().trim() == rhs.getFullPathName().trim();
}

juce::String sceneTransitionEndSampleEditorContext(MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int chainLength = processor.getSceneChainLength();
    if (stepIndex < 0 || stepIndex >= chainLength)
        return {};

    const bool loopbackStep = processor.isSceneChainLoopEnabled() && stepIndex == (chainLength - 1);
    const int fromSceneSlot = processor.getSceneChainStepSceneSlot(stepIndex);
    const int nextStep = (stepIndex + 1 < chainLength)
        ? (stepIndex + 1)
        : (processor.isSceneChainLoopEnabled() ? processor.getSceneChainLoopStartStep() : -1);

    juce::String label = loopbackStep ? "Loopback clamp" : "Fill bubble";
    label << "  Step " << juce::String(stepIndex + 1);
    if (fromSceneSlot >= 0)
        label << "  S" << juce::String(fromSceneSlot + 1);

    if (nextStep >= 0 && nextStep < chainLength)
    {
        const int toSceneSlot = processor.getSceneChainStepSceneSlot(nextStep);
        if (toSceneSlot >= 0)
            label << " -> S" << juce::String(toSceneSlot + 1);
    }
    else
    {
        label << " -> Stop";
    }

    return label;
}

juce::String sceneTransitionEndSampleFrequencyLabel(double value, bool lowpass)
{
    if ((lowpass && value >= 19950.0) || (!lowpass && value <= 20.5))
        return "Off";

    if (value >= 1000.0)
        return juce::String(value / 1000.0, value >= 10000.0 ? 0 : 1) + " kHz";

    return juce::String(juce::roundToInt(value)) + " Hz";
}

juce::String sceneTransitionEndSampleDuckSourceSummary(int sourceSelection)
{
    if (sourceSelection <= 0)
        return "Off";
    if (sourceSelection == 1)
        return "Master";

    return "Strip " + juce::String(sourceSelection - 1);
}
} // namespace

SceneTransitionEndSampleEditorPanel::SceneTransitionEndSampleEditorPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    titleLabel.setText("Fill-End Sample", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    contextLabel.setColour(juce::Label::textColourId, kTextMuted);
    contextLabel.setFont(juce::Font(juce::FontOptions(9.8f, juce::Font::bold)));
    contextLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(contextLabel);

    closeButton.setButtonText("Close");
    closeButton.setTooltip("Close the fill-end sample editor.");
    closeButton.onClick = [this]()
    {
        if (onCloseRequested != nullptr)
            onCloseRequested();
    };
    styleUiButton(closeButton);
    addAndMakeVisible(closeButton);

    currentSampleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    currentSampleLabel.setFont(juce::Font(juce::FontOptions(11.4f, juce::Font::bold)));
    currentSampleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentSampleLabel);

    folderLabel.setColour(juce::Label::textColourId, kTextSecondary);
    folderLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    folderLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(folderLabel);

    folderButton.setButtonText("Folder");
    folderButton.setTooltip("Choose the folder used for fill-end sample browsing.");
    folderButton.onClick = [this]()
    {
        auto startDir = listedDirectory;
        if (!sceneTransitionEndSampleDirectoryAvailable(startDir))
            startDir = processor.getSceneTransitionEndSampleDirectory();
        if (!sceneTransitionEndSampleDirectoryAvailable(startDir))
            startDir = effectiveSceneTransitionEndSampleDirectory(processor, pinnedStepIndex);
        if (!sceneTransitionEndSampleDirectoryAvailable(startDir))
            startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

        juce::FileChooser chooser("Select fill-end sample folder", startDir, "*");
        if (!chooser.browseForDirectory())
            return;

        listedDirectory = chooser.getResult();
        processor.setSceneTransitionEndSampleDirectory(listedDirectory);
        rebuildSampleChoices();
        refreshFromProcessor();
        if (onChanged != nullptr)
            onChanged();
    };
    styleUiButton(folderButton);
    addAndMakeVisible(folderButton);

    clearButton.setButtonText("Clear Pick");
    clearButton.setTooltip("Remove the picked fill-end sample from this bubble.");
    clearButton.onClick = [this]()
    {
        if (pinnedStepIndex < 0)
            return;

        processor.setSceneChainStepTransitionEndSampleFile(pinnedStepIndex, {});
        refreshFromProcessor();
        if (onChanged != nullptr)
            onChanged();
    };
    styleUiButton(clearButton);
    addAndMakeVisible(clearButton);

    sampleListLabel.setColour(juce::Label::textColourId, kTextMuted);
    sampleListLabel.setFont(juce::Font(juce::FontOptions(9.6f, juce::Font::bold)));
    sampleListLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sampleListLabel);

    sampleListBox.setModel(this);
    sampleListBox.setRowHeight(24);
    sampleListBox.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff171a1d));
    sampleListBox.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff596068));
    sampleListBox.setColour(juce::ListBox::textColourId, kTextPrimary);
    sampleListBox.setOutlineThickness(1);
    addAndMakeVisible(sampleListBox);

    shapeLabel.setText("Playback Shape", juce::dontSendNotification);
    shapeLabel.setColour(juce::Label::textColourId, kTextSecondary);
    shapeLabel.setFont(juce::Font(juce::FontOptions(9.6f, juce::Font::bold)));
    shapeLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(shapeLabel);

    auto styleLabel = [](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, kTextMuted);
        label.setFont(juce::Font(juce::FontOptions(10.0f)));
        label.setJustificationType(juce::Justification::centredLeft);
    };

    auto styleSlider = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 16);
        slider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff23282d));
        slider.setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.82f));
        slider.setColour(juce::Slider::thumbColourId, juce::Colours::white.withAlpha(0.94f));
        slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff1b1f22));
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff596068));
        slider.setColour(juce::Slider::textBoxTextColourId, kTextPrimary);
    };

    auto connectSlider = [this, &styleLabel, &styleSlider](juce::Label& label,
                                                           juce::Slider& slider,
                                                           const juce::String& text,
                                                           double minValue,
                                                           double maxValue,
                                                           double interval,
                                                           const juce::String& tooltip)
    {
        styleLabel(label, text);
        addAndMakeVisible(label);
        slider.setRange(minValue, maxValue, interval);
        slider.setTooltip(tooltip);
        slider.onValueChange = [this]()
        {
            if (!suppressControlCallbacks)
                applyCurrentSettings();
        };
        styleSlider(slider);
        addAndMakeVisible(slider);
    };

    connectSlider(gainLabel,
                  gainSlider,
                  "Gain",
                  -24.0,
                  12.0,
                  0.1,
                  "Trim the picked end sample louder or quieter.");
    gainSlider.textFromValueFunction = [](double value)
    {
        return (value > 0.0 ? "+" : "") + juce::String(value, 1) + " dB";
    };

    connectSlider(pitchLabel,
                  pitchSlider,
                  "Pitch",
                  -24.0,
                  24.0,
                  0.1,
                  "Transpose the picked end sample in semitones.");
    pitchSlider.textFromValueFunction = [](double value)
    {
        return (value > 0.0 ? "+" : "") + juce::String(value, 1) + " st";
    };

    connectSlider(fadeInLabel,
                  fadeInSlider,
                  "Fade In",
                  0.0,
                  500.0,
                  1.0,
                  "Fade the start of the picked end sample.");
    fadeInSlider.textFromValueFunction = [](double value)
    {
        return juce::String(juce::roundToInt(value)) + " ms";
    };

    connectSlider(fadeOutLabel,
                  fadeOutSlider,
                  "Fade Out",
                  0.0,
                  500.0,
                  1.0,
                  "Fade the end of the picked end sample.");
    fadeOutSlider.textFromValueFunction = [](double value)
    {
        return juce::String(juce::roundToInt(value)) + " ms";
    };

    connectSlider(lowpassLabel,
                  lowpassSlider,
                  "LPF",
                  20.0,
                  20000.0,
                  1.0,
                  "Low-pass filter for the picked end sample. Turn fully up for Off.");
    lowpassSlider.setSkewFactorFromMidPoint(2200.0);
    lowpassSlider.textFromValueFunction = [](double value)
    {
        return sceneTransitionEndSampleFrequencyLabel(value, true);
    };

    connectSlider(highpassLabel,
                  highpassSlider,
                  "HPF",
                  20.0,
                  20000.0,
                  1.0,
                  "High-pass filter for the picked end sample. Turn fully down for Off.");
    highpassSlider.setSkewFactorFromMidPoint(180.0);
    highpassSlider.textFromValueFunction = [](double value)
    {
        return sceneTransitionEndSampleFrequencyLabel(value, false);
    };

    styleLabel(duckLabel, "Duck");
    addAndMakeVisible(duckLabel);

    duckSourceBox.addItem("Off", 1);
    duckSourceBox.addItem("Master", 2);
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
        duckSourceBox.addItem("S" + juce::String(stripIndex + 1), stripIndex + 3);
    duckSourceBox.setTooltip("Auto-duck the end sample from the chosen strip or the master mix.");
    duckSourceBox.onChange = [this]()
    {
        if (!suppressControlCallbacks)
            applyCurrentSettings();
    };
    styleUiCombo(duckSourceBox);
    addAndMakeVisible(duckSourceBox);

    duckActivityIndicator.setText({}, juce::dontSendNotification);
    duckActivityIndicator.setOpaque(true);
    duckActivityIndicator.setBorderSize({1, 1, 1, 1});
    duckActivityIndicator.setInterceptsMouseClicks(false, false);
    duckActivityIndicator.setTooltip("Duck sidechain activity.");
    duckActivityIndicator.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.10f));
    duckActivityIndicator.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.08f));
    addAndMakeVisible(duckActivityIndicator);

    connectSlider(duckAmountLabel,
                  duckAmountSlider,
                  "Amt",
                  0.0,
                  1.0,
                  0.01,
                  "How strongly the chosen sidechain source ducks the end sample.");
    duckAmountSlider.textFromValueFunction = [](double value)
    {
        return juce::String(juce::roundToInt(value * 100.0)) + "%";
    };

    chokeToggle.setButtonText("Choke previous");
    chokeToggle.setTooltip("Stop any previous end sample voice when this one fires.");
    chokeToggle.onClick = [this]()
    {
        if (!suppressControlCallbacks)
            applyCurrentSettings();
    };
    chokeToggle.setColour(juce::ToggleButton::textColourId, kTextPrimary);
    chokeToggle.setColour(juce::ToggleButton::tickColourId, kAccent);
    addAndMakeVisible(chokeToggle);

    reverseToggle.setButtonText("Reverse");
    reverseToggle.setTooltip("Play the picked end sample backwards.");
    reverseToggle.onClick = [this]()
    {
        if (!suppressControlCallbacks)
            applyCurrentSettings();
    };
    reverseToggle.setColour(juce::ToggleButton::textColourId, kTextPrimary);
    reverseToggle.setColour(juce::ToggleButton::tickColourId, kAccent);
    addAndMakeVisible(reverseToggle);

    hintLabel.setColour(juce::Label::textColourId, kTextMuted);
    hintLabel.setFont(juce::Font(juce::FontOptions(9.6f)));
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    hintLabel.setText("Click a sample to audition and assign it. Duck can follow master or any strip.",
                      juce::dontSendNotification);
    addAndMakeVisible(hintLabel);

    startTimer(180);
    refreshFromProcessor();
}

void SceneTransitionEndSampleEditorPanel::setPinnedStep(int stepIndex, const juce::String& contextText)
{
    pinnedStepIndex = stepIndex;
    pinnedContextText = contextText;
    refreshFromProcessor();
}

void SceneTransitionEndSampleEditorPanel::clearPinnedStep()
{
    pinnedStepIndex = -1;
    pinnedContextText.clear();
    sampleChoices.clear();
    sampleListBox.updateContent();
    sampleListBox.deselectAllRows();
    refreshFromProcessor();
}

void SceneTransitionEndSampleEditorPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void SceneTransitionEndSampleEditorPanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    auto titleRow = bounds.removeFromTop(22);
    closeButton.setBounds(titleRow.removeFromRight(60));
    titleLabel.setBounds(titleRow);

    bounds.removeFromTop(2);
    contextLabel.setBounds(bounds.removeFromTop(16));
    currentSampleLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(4);

    auto folderRow = bounds.removeFromTop(22);
    clearButton.setBounds(folderRow.removeFromRight(84));
    folderRow.removeFromRight(4);
    folderButton.setBounds(folderRow.removeFromRight(64));
    folderRow.removeFromRight(8);
    folderLabel.setBounds(folderRow);

    bounds.removeFromTop(6);
    sampleListLabel.setBounds(bounds.removeFromTop(16));
    sampleListBox.setBounds(bounds.removeFromTop(98));

    bounds.removeFromTop(8);
    shapeLabel.setBounds(bounds.removeFromTop(16));

    auto toggleRow = bounds.removeFromTop(22);
    chokeToggle.setBounds(toggleRow.removeFromLeft(toggleRow.getWidth() / 2));
    reverseToggle.setBounds(toggleRow);

    bounds.removeFromTop(4);
    auto duckRow = bounds.removeFromTop(22);
    duckLabel.setBounds(duckRow.removeFromLeft(54));
    duckSourceBox.setBounds(duckRow.removeFromLeft(90));
    duckRow.removeFromLeft(6);
    duckActivityIndicator.setBounds(duckRow.removeFromLeft(14).reduced(0, 4));
    duckRow.removeFromLeft(6);
    duckAmountLabel.setBounds(duckRow.removeFromLeft(36));
    duckAmountSlider.setBounds(duckRow);

    bounds.removeFromTop(4);
    auto layoutSliderRow = [&](juce::Label& label, juce::Slider& slider)
    {
        auto row = bounds.removeFromTop(22);
        label.setBounds(row.removeFromLeft(54));
        slider.setBounds(row);
        bounds.removeFromTop(2);
    };

    layoutSliderRow(gainLabel, gainSlider);
    layoutSliderRow(pitchLabel, pitchSlider);
    layoutSliderRow(fadeInLabel, fadeInSlider);
    layoutSliderRow(fadeOutLabel, fadeOutSlider);
    layoutSliderRow(lowpassLabel, lowpassSlider);
    layoutSliderRow(highpassLabel, highpassSlider);

    hintLabel.setBounds(bounds);
}

int SceneTransitionEndSampleEditorPanel::getNumRows()
{
    return static_cast<int>(sampleChoices.size());
}

void SceneTransitionEndSampleEditorPanel::paintListBoxItem(int rowNumber,
                                                           juce::Graphics& g,
                                                           int width,
                                                           int height,
                                                           bool rowIsSelected)
{
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(4, 2);
    if (rowNumber < 0 || rowNumber >= static_cast<int>(sampleChoices.size()) || bounds.isEmpty())
        return;

    const auto& file = sampleChoices[static_cast<size_t>(rowNumber)];
    const bool isPicked = pinnedStepIndex >= 0
        && sceneTransitionEndSampleFilesMatch(processor.getSceneChainStepTransitionEndSampleFile(pinnedStepIndex),
                                              file);
    const auto fillColour = rowIsSelected
        ? kAccent.withAlpha(0.26f)
        : (isPicked ? kAccent.withAlpha(0.16f) : juce::Colours::black.withAlpha(0.14f));
    const auto outlineColour = rowIsSelected || isPicked
        ? kAccent.withAlpha(rowIsSelected ? 0.88f : 0.48f)
        : juce::Colours::white.withAlpha(0.08f);

    g.setColour(fillColour);
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(outlineColour);
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    auto textBounds = bounds.reduced(8, 0);
    if (isPicked)
    {
        auto badgeBounds = textBounds.removeFromRight(58).reduced(0, 3);
        g.setColour(kAccent.withAlpha(0.92f));
        g.fillRoundedRectangle(badgeBounds.toFloat(), 4.0f);
        g.setColour(juce::Colour(0xff111111));
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawFittedText("PICKED", badgeBounds, juce::Justification::centred, 1);
        textBounds.removeFromRight(4);
    }

    g.setColour(kTextPrimary.withAlpha(rowIsSelected ? 0.98f : 0.92f));
    g.setFont(juce::Font(juce::FontOptions(10.6f)));
    g.drawFittedText(sampleChoiceLabel(file), textBounds, juce::Justification::centredLeft, 1);
}

void SceneTransitionEndSampleEditorPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (!suppressListCallbacks)
        auditionAndPickRow(row);
}

void SceneTransitionEndSampleEditorPanel::selectedRowsChanged(int lastRowSelected)
{
    if (suppressListCallbacks || lastRowSelected < 0 || !sampleListBox.hasKeyboardFocus(true))
        return;

    auditionAndPickRow(lastRowSelected);
}

void SceneTransitionEndSampleEditorPanel::timerCallback()
{
    if (!isShowing())
        return;

    const bool draggingSlider = gainSlider.isMouseButtonDown()
        || pitchSlider.isMouseButtonDown()
        || fadeInSlider.isMouseButtonDown()
        || fadeOutSlider.isMouseButtonDown()
        || lowpassSlider.isMouseButtonDown()
        || highpassSlider.isMouseButtonDown()
        || duckAmountSlider.isMouseButtonDown();
    if (!draggingSlider)
        refreshFromProcessor();
    else
        refreshDuckActivityIndicator();
}

void SceneTransitionEndSampleEditorPanel::refreshFromProcessor()
{
    if (pinnedStepIndex < 0 || pinnedStepIndex >= processor.getSceneChainLength())
    {
        contextLabel.setText("Pick a fill bubble to edit its end sample.", juce::dontSendNotification);
        currentSampleLabel.setText("Picked: None", juce::dontSendNotification);
        currentSampleLabel.setTooltip({});
        folderLabel.setText("Folder: not set", juce::dontSendNotification);
        folderLabel.setTooltip({});
        sampleListLabel.setText("Choose a folder to browse end samples.", juce::dontSendNotification);
        clearButton.setEnabled(false);
        folderButton.setEnabled(false);
        sampleChoices.clear();
        sampleListBox.updateContent();
        sampleListBox.deselectAllRows();
        refreshDuckActivityIndicator();
        return;
    }

    folderButton.setEnabled(true);
    contextLabel.setText(pinnedContextText, juce::dontSendNotification);

    const auto pickedFile = processor.getSceneChainStepTransitionEndSampleFile(pinnedStepIndex);
    currentSampleLabel.setText(sceneTransitionEndSamplePickedLabel(pickedFile), juce::dontSendNotification);
    currentSampleLabel.setTooltip(pickedFile.getFullPathName());
    clearButton.setEnabled(pickedFile != juce::File());

    auto directory = listedDirectory;
    if (!sceneTransitionEndSampleDirectoryAvailable(directory))
        directory = effectiveSceneTransitionEndSampleDirectory(processor, pinnedStepIndex);
    if (directory != listedDirectory)
    {
        listedDirectory = directory;
        rebuildSampleChoices();
    }

    if (sceneTransitionEndSampleDirectoryAvailable(directory))
    {
        folderLabel.setText("Folder: " + directory.getFullPathName(), juce::dontSendNotification);
        folderLabel.setTooltip(directory.getFullPathName());
    }
    else
    {
        folderLabel.setText("Folder: not set", juce::dontSendNotification);
        folderLabel.setTooltip({});
    }

    const auto settings = processor.getSceneChainStepTransitionEndSampleSettings(pinnedStepIndex);
    const juce::ScopedValueSetter<bool> callbackGuard(suppressControlCallbacks, true);
    gainSlider.setValue(settings.gainDb, juce::dontSendNotification);
    pitchSlider.setValue(settings.pitchSemitones, juce::dontSendNotification);
    fadeInSlider.setValue(settings.fadeInMs, juce::dontSendNotification);
    fadeOutSlider.setValue(settings.fadeOutMs, juce::dontSendNotification);
    lowpassSlider.setValue(settings.lowpassHz, juce::dontSendNotification);
    highpassSlider.setValue(settings.highpassHz, juce::dontSendNotification);
    duckSourceBox.setSelectedId(settings.duckSource + 1, juce::dontSendNotification);
    duckAmountSlider.setValue(settings.duckAmount, juce::dontSendNotification);
    chokeToggle.setToggleState(settings.chokePrevious, juce::dontSendNotification);
    reverseToggle.setToggleState(settings.reverse, juce::dontSendNotification);

    int selectedRow = -1;
    for (int index = 0; index < static_cast<int>(sampleChoices.size()); ++index)
    {
        if (sceneTransitionEndSampleFilesMatch(sampleChoices[static_cast<size_t>(index)], pickedFile))
        {
            selectedRow = index;
            break;
        }
    }

    juce::String listSummary;
    if (!directory.exists() || !directory.isDirectory())
        listSummary = "Choose a folder to browse end samples.";
    else if (sampleChoices.empty())
        listSummary = "No supported audio files found here yet.";
    else
        listSummary = juce::String(sampleChoices.size()) + " files  •  click to audition + pick";
    sampleListLabel.setText(listSummary, juce::dontSendNotification);

    const juce::ScopedValueSetter<bool> listGuard(suppressListCallbacks, true);
    sampleListBox.updateContent();
    if (selectedRow >= 0)
        sampleListBox.selectRow(selectedRow, false, true);
    else
        sampleListBox.deselectAllRows();
    sampleListBox.repaint();
    refreshDuckActivityIndicator();
}

void SceneTransitionEndSampleEditorPanel::rebuildSampleChoices()
{
    sampleChoices = collectSceneTransitionEndSampleChoices(listedDirectory);
}

void SceneTransitionEndSampleEditorPanel::refreshDuckActivityIndicator()
{
    int duckSource = juce::jmax(0, duckSourceBox.getSelectedId() - 1);
    float duckAmount = static_cast<float>(duckAmountSlider.getValue());
    if (pinnedStepIndex >= 0 && pinnedStepIndex < processor.getSceneChainLength())
    {
        const auto settings = processor.getSceneChainStepTransitionEndSampleSettings(pinnedStepIndex);
        duckSource = settings.duckSource;
        duckAmount = settings.duckAmount;
    }

    float activity = 0.0f;
    if (duckSource > 0 && duckAmount > 0.001f)
    {
        if (const auto* engine = processor.getAudioEngine())
        {
            if (duckSource == 1)
                activity = engine->getMasterOutputLevel();
            else
                activity = engine->getStripOutputLevel(duckSource - 2);
        }
    }

    const bool active = duckSource > 0 && duckAmount > 0.001f;
    const float shapedActivity = juce::jlimit(0.0f, 1.0f, std::sqrt(juce::jmax(0.0f, activity * 1.8f)));
    const auto fillColour = active
        ? kAccent.interpolatedWith(juce::Colours::white, 0.18f).withAlpha(0.18f + (0.78f * shapedActivity))
        : juce::Colours::black.withAlpha(0.10f);
    const auto outlineColour = active
        ? kAccent.withAlpha(0.30f + (0.56f * shapedActivity))
        : juce::Colours::white.withAlpha(0.08f);
    duckActivityIndicator.setColour(juce::Label::backgroundColourId, fillColour);
    duckActivityIndicator.setColour(juce::Label::outlineColourId, outlineColour);
    duckActivityIndicator.setTooltip(active
                                         ? ("Duck sidechain activity from "
                                            + sceneTransitionEndSampleDuckSourceSummary(duckSource)
                                            + ".")
                                         : "Duck is off.");
    duckActivityIndicator.repaint();
}

juce::String SceneTransitionEndSampleEditorPanel::sampleChoiceLabel(const juce::File& file) const
{
    return sceneTransitionEndSampleChoiceLabel(file, listedDirectory);
}

void SceneTransitionEndSampleEditorPanel::auditionAndPickRow(int row)
{
    if (pinnedStepIndex < 0 || row < 0 || row >= static_cast<int>(sampleChoices.size()))
        return;

    const auto file = sampleChoices[static_cast<size_t>(row)];
    const auto settings = processor.getSceneChainStepTransitionEndSampleSettings(pinnedStepIndex);
    processor.setSceneTransitionEndSampleDirectory(file.getParentDirectory());
    processor.setSceneChainStepTransitionEndSampleFile(pinnedStepIndex, file);
    processor.auditionSceneTransitionEndSampleFile(file, settings);
    refreshFromProcessor();
    if (onChanged != nullptr)
        onChanged();
}

void SceneTransitionEndSampleEditorPanel::applyCurrentSettings()
{
    if (pinnedStepIndex < 0)
        return;

    MlrVSTAudioProcessor::SceneTransitionEndSampleSettings settings;
    settings.gainDb = static_cast<float>(gainSlider.getValue());
    settings.fadeInMs = static_cast<float>(fadeInSlider.getValue());
    settings.fadeOutMs = static_cast<float>(fadeOutSlider.getValue());
    settings.chokePrevious = chokeToggle.getToggleState();
    settings.reverse = reverseToggle.getToggleState();
    settings.pitchSemitones = static_cast<float>(pitchSlider.getValue());
    settings.lowpassHz = static_cast<float>(lowpassSlider.getValue());
    settings.highpassHz = static_cast<float>(highpassSlider.getValue());
    settings.duckSource = juce::jmax(0, duckSourceBox.getSelectedId() - 1);
    settings.duckAmount = static_cast<float>(duckAmountSlider.getValue());
    processor.setSceneChainStepTransitionEndSampleSettings(pinnedStepIndex, settings);
    refreshDuckActivityIndicator();
}

SceneControlPanel::SceneControlPanel(MlrVSTAudioProcessor& p)
    : processor(p),
      sceneChainCanvas(*this),
      sceneTimelineCanvas(*this)
{
    setWantsKeyboardFocus(true);
    sceneGridEnabled = processor.getSceneEditorGridEnabled();
    sceneGridDivision = processor.getSceneEditorGridDivision();
    sceneZoomFactor = processor.getSceneEditorZoomFactor();
    sceneDrawModeEnabled = processor.getSceneEditorDrawModeEnabled();
    sceneFollowPlayheadEnabled = processor.getSceneEditorFollowPlayheadEnabled();
    sceneLaneOverlayEnabled = processor.getSceneEditorLaneOverlaysEnabled();
    for (int stripIndex = 0; stripIndex < SceneEditorVisibleStrips; ++stripIndex)
    {
        stripAutomationExpanded[static_cast<size_t>(stripIndex)] =
            processor.getSceneEditorStripAutomationExpanded(stripIndex);
        stripHeightExpanded[static_cast<size_t>(stripIndex)] =
            processor.getSceneEditorStripHeightExpanded(stripIndex);
    }

    auto styleSceneCombo = [](juce::ComboBox& combo)
    {
        combo.setLookAndFeel(nullptr);
        styleUiCombo(combo);
        combo.setJustificationType(juce::Justification::centredLeft);
    };

    auto styleSceneButton = [this](juce::Button& button,
                                   const char* role,
                                   juce::Colour offColour,
                                   juce::Colour onColour,
                                   juce::Colour offText = kTextPrimary,
                                   juce::Colour onText = juce::Colour(0xfff7f7f7))
    {
        button.setLookAndFeel(&sceneLookAndFeel);
        button.getProperties().set("sceneRole", role);
        button.setColour(juce::TextButton::buttonColourId, offColour);
        button.setColour(juce::TextButton::buttonOnColourId, onColour);
        button.setColour(juce::TextButton::textColourOffId, offText);
        button.setColour(juce::TextButton::textColourOnId, onText);
    };

    auto styleScenePrimaryButton = [&](juce::Button& button, const char* role)
    {
        styleSceneButton(button,
                         role,
                         juce::Colour(0xff374048),
                         kAccent.withAlpha(0.94f),
                         kTextPrimary,
                         juce::Colour(0xff101010));
    };

    auto styleSceneUtilityButton = [&](juce::Button& button)
    {
        styleSceneButton(button,
                         "chip",
                         juce::Colour(0xff272d33),
                         juce::Colour(0xff404a53),
                         juce::Colour(0xffdbe1e6),
                         juce::Colour(0xfff5f7f8));
    };

    auto styleSceneToolButton = [&](juce::Button& button, bool accentOn = false)
    {
        if (accentOn)
        {
            styleSceneButton(button,
                             "tool",
                             juce::Colour(0xff303840),
                             kAccent.withAlpha(0.90f),
                             kTextPrimary,
                             juce::Colour(0xff101010));
            return;
        }

        styleSceneButton(button,
                         "tool",
                         juce::Colour(0xff2a3138),
                         juce::Colour(0xff46525c),
                         juce::Colour(0xffd6dde2),
                         juce::Colour(0xfff4f7f8));
    };

    auto styleSceneTransitionSlider = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setPopupDisplayEnabled(true, false, nullptr);
        slider.setScrollWheelEnabled(false);
        slider.setNumDecimalPlacesToDisplay(0);
        slider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff242b30));
        slider.setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.82f));
        slider.setColour(juce::Slider::thumbColourId, juce::Colours::white.withAlpha(0.94f));
    };

    auto configureHeaderLabel = [](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, kTextMuted);
        label.setJustificationType(juce::Justification::centredLeft);
    };

    auto configureSectionLabel = [](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, juce::Colour(0xffaeb8c0));
        label.setJustificationType(juce::Justification::centredLeft);
    };

    auto configureSummaryLabel = [](juce::Label& label, bool compact)
    {
        label.setFont(juce::Font(juce::FontOptions(compact ? 9.2f : 9.7f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, compact ? kTextSecondary : kTextPrimary);
        label.setColour(juce::Label::backgroundColourId, juce::Colour(0xff1b2125));
        label.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.10f));
        label.setBorderSize({2, 7, 2, 7});
        label.setJustificationType(juce::Justification::centredLeft);
    };

    titleLabel.setText("SCENE", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    hintLabel.setText("Pick a scene, set how it advances, then shape the chain and fills below.", juce::dontSendNotification);
    hintLabel.setFont(juce::Font(juce::FontOptions(10.2f)));
    hintLabel.setColour(juce::Label::textColourId, kTextMuted);
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    hintLabel.setTooltip("Scene buttons focus and launch scenes. Playback controls decide when the next scene starts. The chain rail sets order, repeats, and fill bubbles between scenes.");
    addAndMakeVisible(hintLabel);

    sceneModeToggle.setButtonText("Scene Mode");
    sceneModeToggle.setClickingTogglesState(true);
    sceneModeToggle.setTooltip("Enable scene mode: recalls stored scene snapshots and opens the full scene performance workspace.");
    sceneModeToggle.onClick = [this]()
    {
        processor.setSceneModeEnabled(sceneModeToggle.getToggleState());
    };
    addAndMakeVisible(sceneModeToggle);
    styleSceneToolButton(sceneModeToggle);

    configureSectionLabel(sceneSlotsSectionLabel, "SCENES");
    addAndMakeVisible(sceneSlotsSectionLabel);
    sceneSlotsSectionLabel.setVisible(false);

    configureSectionLabel(scenePlaybackSectionLabel, "PLAYBACK");
    addAndMakeVisible(scenePlaybackSectionLabel);
    scenePlaybackSectionLabel.setVisible(false);

    configureSectionLabel(sceneFillSectionLabel, "FILL");
    addAndMakeVisible(sceneFillSectionLabel);
    sceneFillSectionLabel.setVisible(false);

    sceneChangeModeLabel.setText("Next", juce::dontSendNotification);
    sceneChangeModeLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    sceneChangeModeLabel.setColour(juce::Label::textColourId, kTextMuted);
    sceneChangeModeLabel.setJustificationType(juce::Justification::centredLeft);
    sceneChangeModeLabel.setTooltip("Choose when the chain should move to the next scene.");
    addAndMakeVisible(sceneChangeModeLabel);

    sceneChangeModeBox.addItem("Grid", static_cast<int>(MlrVSTAudioProcessor::SceneRecallMode::QuantizeGrid) + 1);
    sceneChangeModeBox.addItem("Pattern", static_cast<int>(MlrVSTAudioProcessor::SceneRecallMode::PatternEnd) + 1);
    sceneChangeModeBox.addItem("Scene", static_cast<int>(MlrVSTAudioProcessor::SceneRecallMode::SceneEnd) + 1);
    sceneChangeModeBox.addItem("Manual", static_cast<int>(MlrVSTAudioProcessor::SceneRecallMode::Manual) + 1);
    sceneChangeModeBox.setTooltip("Grid waits for the next launch quantize point. Pattern waits for the active loops to finish. Scene waits for this scene's Length and Repeats. Manual never auto-advances.");
    sceneChangeModeBox.onChange = [this]()
    {
        const int selectedId = sceneChangeModeBox.getSelectedId();
        if (selectedId <= 0)
            return;

        processor.setSceneRecallMode(static_cast<MlrVSTAudioProcessor::SceneRecallMode>(selectedId - 1));
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneChangeModeBox);
    styleSceneCombo(sceneChangeModeBox);

    configureHeaderLabel(sceneSlotHeaderLabel, "Current");
    addAndMakeVisible(sceneSlotHeaderLabel);
    sceneSlotHeaderLabel.setVisible(false);

    sceneSceneCaptureButton.setButtonText("Save");
    sceneSceneCaptureButton.setTooltip("Save the current live state into the focused scene slot.");
    sceneSceneCaptureButton.onClick = [this]()
    {
        processor.captureSceneSlot(getFocusedSceneSlot());
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneSceneCaptureButton);
    styleSceneUtilityButton(sceneSceneCaptureButton);
    sceneSceneCaptureButton.setVisible(false);

    sceneSceneCopyButton.setButtonText("Copy");
    sceneSceneCopyButton.setTooltip("Copy the focused scene so it can be pasted into another scene slot.");
    sceneSceneCopyButton.onClick = [this]()
    {
        if (processor.copySceneSlotToClipboard(getFocusedSceneSlot()))
            refreshFromProcessor();
    };
    addAndMakeVisible(sceneSceneCopyButton);
    styleSceneUtilityButton(sceneSceneCopyButton);

    sceneScenePasteButton.setButtonText("Paste");
    sceneScenePasteButton.setTooltip("Paste the copied scene into the focused scene slot.");
    sceneScenePasteButton.onClick = [this]()
    {
        if (processor.pasteSceneSlotFromClipboard(getFocusedSceneSlot()))
            refreshFromProcessor();
    };
    addAndMakeVisible(sceneScenePasteButton);
    styleSceneUtilityButton(sceneScenePasteButton);

    sceneChainPlayButton.setButtonText("Play");
    sceneChainPlayButton.setClickingTogglesState(true);
    sceneChainPlayButton.setTooltip("Start or stop chain playback. If a chain scene is already live while transport is running, the chain attaches there without retriggering it.");
    sceneChainPlayButton.onClick = [this]()
    {
        const int chainLength = processor.getSceneChainLength();
        const bool shouldPlay = sceneChainPlayButton.getToggleState();
        if (!shouldPlay)
        {
            processor.stopSceneChainPlayback();
            refreshFromProcessor();
            return;
        }

        if (chainLength < 2)
        {
            sceneChainPlayButton.setToggleState(false, juce::dontSendNotification);
            refreshFromProcessor();
            return;
        }

        int startStep = processor.getSceneSequenceStepIndex(getFocusedSceneSlot());
        if (startStep < 0)
            startStep = processor.getSceneSequenceStepIndex(processor.getActiveSceneSlot());
        if (startStep < 0)
            startStep = 0;

        if (!processor.startSceneChainPlayback(startStep))
            sceneChainPlayButton.setToggleState(false, juce::dontSendNotification);
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneChainPlayButton);
    styleScenePrimaryButton(sceneChainPlayButton, "transport");

    sceneChainClearButton.setButtonText("Reset");
    sceneChainClearButton.setTooltip("Reset the focused scene slot to its default empty state. Alt-click clears the chain.");
    sceneChainClearButton.onClick = [this]()
    {
        const auto modifiers = juce::ModifierKeys::getCurrentModifiersRealtime();
        if (modifiers.isAltDown())
        {
            processor.clearSceneChain();
            refreshFromProcessor();
            return;
        }

        selectedSceneActionSlot = getFocusedSceneSlot();
        processor.focusSceneSlot(selectedSceneActionSlot);
        processor.clearSceneSlot(selectedSceneActionSlot);
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneChainClearButton);
    styleSceneUtilityButton(sceneChainClearButton);

    sceneCaptureButton.setButtonText("Draw");
    sceneCaptureButton.setClickingTogglesState(true);
    sceneCaptureButton.setTooltip("With Draw off, click automation points to place anchors and connect line segments between clicks. Enable Draw to paint triggers and automation directly into the clip grid. Option-click resets automation points to the lane default, Ctrl/Cmd-click defaults a whole automation lane, and Option-drag snaps bipolar lanes to center.");
    sceneCaptureButton.onClick = [this]()
    {
        sceneDrawModeEnabled = sceneCaptureButton.getToggleState();
        sceneEditorState.clickLinePending = false;
        sceneEditorState.clickLineStripIndex = -1;
        sceneEditorState.clickLineLaneIndex = -1;
        sceneEditorState.clickLineBeat = 0.0;
        sceneEditorState.clickLineValue = 0.0f;
        processor.setSceneEditorDrawModeEnabled(sceneDrawModeEnabled);
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneCaptureButton);
    styleSceneToolButton(sceneCaptureButton, true);

    sceneInsertBeforeButton.setButtonText("Before");
    sceneInsertBeforeButton.setTooltip("Shift later scenes right and capture the current live state before the selected slot.");
    sceneInsertBeforeButton.onClick = [this]()
    {
        const int targetSlot = getFocusedSceneSlot();
        if (processor.insertSceneSlot(targetSlot, false))
        {
            selectedSceneActionSlot = targetSlot;
            processor.focusSceneSlot(selectedSceneActionSlot);
        }
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneInsertBeforeButton);
    styleSceneUtilityButton(sceneInsertBeforeButton);

    sceneInsertAfterButton.setButtonText("After");
    sceneInsertAfterButton.setTooltip("Shift later scenes right and capture the current live state after the selected slot.");
    sceneInsertAfterButton.onClick = [this]()
    {
        const int targetSlot = getFocusedSceneSlot();
        if (processor.insertSceneSlot(targetSlot, true))
        {
            selectedSceneActionSlot = juce::jmin(MlrVSTAudioProcessor::SceneSlots - 1, targetSlot + 1);
            processor.focusSceneSlot(selectedSceneActionSlot);
        }
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneInsertAfterButton);
    styleSceneUtilityButton(sceneInsertAfterButton);

    sceneDuplicateLengthButton.setButtonText("Double");
    sceneDuplicateLengthButton.setTooltip("Duplicate the current scene clip into the second half and double the scene length count.");
    sceneDuplicateLengthButton.onClick = [this]()
    {
        duplicateFocusedSceneLength();
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneDuplicateLengthButton);
    styleSceneUtilityButton(sceneDuplicateLengthButton);

    configureHeaderLabel(sceneLengthHeaderLabel, "Length");
    configureHeaderLabel(sceneBarsHeaderLabel, "Repeats");
    configureHeaderLabel(sceneAnchorHeaderLabel, "Ends On");
    configureHeaderLabel(sceneTransitionHeaderLabel, "Type");
    configureHeaderLabel(sceneTransitionOptionsHeaderLabel, "Style");
    configureHeaderLabel(sceneTransitionLengthHeaderLabel, "Lead");
    configureHeaderLabel(sceneTransitionMixHeaderLabel, "Blend");
    configureHeaderLabel(sceneTransitionDelayHeaderLabel, "Echo");
    configureHeaderLabel(sceneTransitionFilterHeaderLabel, "Tone");
    configureHeaderLabel(sceneTransitionChopHeaderLabel, "Gate");
    configureHeaderLabel(sceneTransitionScopeHeaderLabel, "Scope");
    configureHeaderLabel(sceneTransitionContourHeaderLabel, "Contour");
    configureHeaderLabel(sceneTransitionConditionHeaderLabel, "When");
    configureHeaderLabel(sceneTransitionEndSampleHeaderLabel, "End");
    addAndMakeVisible(sceneLengthHeaderLabel);
    addAndMakeVisible(sceneBarsHeaderLabel);
    addAndMakeVisible(sceneAnchorHeaderLabel);
    addAndMakeVisible(sceneTransitionHeaderLabel);
    addAndMakeVisible(sceneTransitionOptionsHeaderLabel);
    addAndMakeVisible(sceneTransitionLengthHeaderLabel);
    addAndMakeVisible(sceneTransitionMixHeaderLabel);
    addAndMakeVisible(sceneTransitionDelayHeaderLabel);
    addAndMakeVisible(sceneTransitionFilterHeaderLabel);
    addAndMakeVisible(sceneTransitionChopHeaderLabel);
    addAndMakeVisible(sceneTransitionScopeHeaderLabel);
    addAndMakeVisible(sceneTransitionContourHeaderLabel);
    addAndMakeVisible(sceneTransitionConditionHeaderLabel);
    addAndMakeVisible(sceneTransitionEndSampleHeaderLabel);
    configureSummaryLabel(sceneTransitionSummaryLabel, false);
    configureSummaryLabel(sceneTransitionMetaLabel, true);
    addAndMakeVisible(sceneTransitionSummaryLabel);
    addAndMakeVisible(sceneTransitionMetaLabel);
    sceneAdvanceSummaryLabel.setVisible(false);
    sceneTransitionSummaryLabel.setVisible(true);
    sceneTransitionMetaLabel.setVisible(true);

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        auto& selectorButton = sceneSelectorButtons[static_cast<size_t>(sceneSlot)];
        auto& lengthModeBox = sceneLengthModeBoxes[static_cast<size_t>(sceneSlot)];
        auto& manualBarsBox = sceneManualBarsBoxes[static_cast<size_t>(sceneSlot)];
        auto& anchorStripBox = sceneAnchorStripBoxes[static_cast<size_t>(sceneSlot)];

        selectorButton.setButtonText("S" + juce::String(sceneSlot + 1));
        selectorButton.setClickingTogglesState(true);
        selectorButton.onClick = [this, sceneSlot]()
        {
            if (sceneSlotDragSuppressClick)
            {
                sceneSlotDragSuppressClick = false;
                refreshFromProcessor();
                return;
            }

            selectedSceneActionSlot = sceneSlot;
            processor.launchSceneSlotFromSurface(sceneSlot);
            refreshFromProcessor();
        };
        addAndMakeVisible(selectorButton);
        styleSceneButton(selectorButton,
                         "slot",
                         juce::Colour(0xff323841),
                         juce::Colour(0xff4b5560),
                         kTextPrimary,
                         juce::Colour(0xfff7f7f7));
        selectorButton.addMouseListener(this, false);

        lengthModeBox.addItem("Longest Strip", static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::LongestStrip) + 1);
        lengthModeBox.addItem("Pattern End", static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::LongestPattern) + 1);
        lengthModeBox.addItem("Bars", static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::ManualBars) + 1);
        lengthModeBox.addItem("Anchor Strip", static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::AnchorStrip) + 1);
        lengthModeBox.setTooltip("Choose what defines this scene's length: the longest loop, longest pattern, a manual bar count, or a specific strip.");
        lengthModeBox.onChange = [this, sceneSlot]()
        {
            const int selectedId = sceneLengthModeBoxes[static_cast<size_t>(sceneSlot)].getSelectedId();
            if (selectedId <= 0)
                return;

            processor.setSceneLengthMode(sceneSlot,
                                         static_cast<MlrVSTAudioProcessor::SceneLengthMode>(selectedId - 1));
            processor.persistSceneTimingForSlot(sceneSlot);
            refreshFromProcessor();
        };
        addAndMakeVisible(lengthModeBox);
        styleSceneCombo(lengthModeBox);

        for (int bars = 1; bars <= MlrVSTAudioProcessor::MaxSceneManualBars; ++bars)
            manualBarsBox.addItem(juce::String(bars), bars);
        manualBarsBox.setTooltip("How many times the chosen scene length should repeat. In Bars mode, this is the number of bars.");
        manualBarsBox.onChange = [this, sceneSlot]()
        {
            const int selectedId = sceneManualBarsBoxes[static_cast<size_t>(sceneSlot)].getSelectedId();
            if (selectedId > 0)
            {
                processor.setSceneLengthCount(sceneSlot, selectedId);
                processor.persistSceneTimingForSlot(sceneSlot);
                refreshFromProcessor();
            }
        };
        addAndMakeVisible(manualBarsBox);
        styleSceneCombo(manualBarsBox);

        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            anchorStripBox.addItem("S" + juce::String(stripIndex + 1), stripIndex + 1);
        anchorStripBox.setTooltip("Choose which strip decides when the scene is allowed to end.");
        anchorStripBox.onChange = [this, sceneSlot]()
        {
            const int selectedId = sceneAnchorStripBoxes[static_cast<size_t>(sceneSlot)].getSelectedId();
            if (selectedId > 0)
            {
                processor.setSceneAnchorStrip(sceneSlot, selectedId - 1);
                processor.persistSceneTimingForSlot(sceneSlot);
                refreshFromProcessor();
            }
        };
        addAndMakeVisible(anchorStripBox);
        styleSceneCombo(anchorStripBox);
    }

    sceneTransitionTypeBox.addItem("None", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::None) + 1);
    sceneTransitionTypeBox.addItem("Fill", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Fill) + 1);
    sceneTransitionTypeBox.addItem("Stutter", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Stutter) + 1);
    sceneTransitionTypeBox.addItem("Rise", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise) + 1);
    sceneTransitionTypeBox.addItem("Drop", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Drop) + 1);
    sceneTransitionTypeBox.addItem("Mute", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::MuteTail) + 1);
    sceneTransitionTypeBox.addItem("Break", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Break) + 1);
    sceneTransitionTypeBox.addItem("Return", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return) + 1);
    sceneTransitionTypeBox.setTooltip("Outgoing connector for the selected chain step. Edit this live while the chain is playing to change the next handoff without stopping the cycle.");
    sceneTransitionTypeBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionTypeBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        const auto selectedType = static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(selectedId - 1);
        processor.setSceneChainStepTransitionType(
            focusedStep,
            selectedType);

        if (sceneTransitionSmartLinkEnabled
            && selectedType != MlrVSTAudioProcessor::SceneChainTransitionType::None
            && selectedType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
        {
            applySceneTransitionSmartTuning(
                focusedStep,
                selectedType,
                processor.getSceneChainStepTransitionOption(focusedStep),
                processor.getSceneChainStepTransitionIntensity(focusedStep),
                processor.getSceneChainStepTransitionLengthBeats(focusedStep),
                processor.getSceneChainStepTransitionSubtractsFromSceneLength(focusedStep),
                false,
                false);
            return;
        }

        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionTypeBox);
    styleSceneCombo(sceneTransitionTypeBox);

    sceneTransitionOptionsBox.addItem("Snap", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Snap) + 1);
    sceneTransitionOptionsBox.addItem("Tight", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Tight) + 1);
    sceneTransitionOptionsBox.addItem("Default", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Default) + 1);
    sceneTransitionOptionsBox.addItem("Wide", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Wide) + 1);
    sceneTransitionOptionsBox.addItem("Wash", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Wash) + 1);
    sceneTransitionOptionsBox.addItem("Echo", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Echo) + 1);
    sceneTransitionOptionsBox.addItem("Sweep", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Sweep) + 1);
    sceneTransitionOptionsBox.addItem("Gate", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Gate) + 1);
    sceneTransitionOptionsBox.setTooltip("Transition profile. Snap/Tight are short, Wide/Wash are longer, Echo leans into delay, Sweep leans into filter motion, and Gate leans into chops/stutter.");
    sceneTransitionOptionsBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionOptionsBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        const auto selectedOption = static_cast<MlrVSTAudioProcessor::SceneChainTransitionOption>(selectedId - 1);
        processor.setSceneChainStepTransitionOption(
            focusedStep,
            selectedOption);

        const auto currentType = processor.getSceneChainStepTransitionType(focusedStep);
        if (sceneTransitionSmartLinkEnabled
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::None
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
        {
            applySceneTransitionSmartTuning(focusedStep,
                                            currentType,
                                            selectedOption,
                                            processor.getSceneChainStepTransitionIntensity(focusedStep),
                                            processor.getSceneChainStepTransitionLengthBeats(focusedStep),
                                            processor.getSceneChainStepTransitionSubtractsFromSceneLength(focusedStep),
                                            false,
                                            false);
            return;
        }

        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionOptionsBox);
    styleSceneCombo(sceneTransitionOptionsBox);

    for (const auto& option : kSceneTransitionLengthOptions)
        sceneTransitionLengthBox.addItem(option.label, option.comboId);
    sceneTransitionLengthBox.setTooltip("Editable fill length in beats before the scene handoff.");
    sceneTransitionLengthBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionLengthBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        const float selectedLength = sceneTransitionValueForComboId(selectedId,
                                                                    kSceneTransitionLengthOptions,
                                                                    MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats);
        processor.setSceneChainStepTransitionLengthBeats(
            focusedStep,
            selectedLength);

        const auto currentType = processor.getSceneChainStepTransitionType(focusedStep);
        if (sceneTransitionSmartLinkEnabled
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::None
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
        {
            applySceneTransitionSmartTuning(focusedStep,
                                            currentType,
                                            processor.getSceneChainStepTransitionOption(focusedStep),
                                            processor.getSceneChainStepTransitionIntensity(focusedStep),
                                            selectedLength,
                                            processor.getSceneChainStepTransitionSubtractsFromSceneLength(focusedStep),
                                            false,
                                            false);
            return;
        }

        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionLengthBox);
    styleSceneCombo(sceneTransitionLengthBox);

    sceneTransitionScopeBox.addItem("All", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::All) + 1);
    sceneTransitionScopeBox.addItem("Loops", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Loops) + 1);
    sceneTransitionScopeBox.addItem("Steps", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Steps) + 1);
    sceneTransitionScopeBox.addItem("Grains", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Grains) + 1);
    sceneTransitionScopeBox.addItem("Flip", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Flip) + 1);
    sceneTransitionScopeBox.setTooltip("Choose which kinds of strips the focused Chain Fill should touch.");
    sceneTransitionScopeBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionScopeBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        processor.setSceneChainStepTransitionScope(
            focusedStep,
            static_cast<MlrVSTAudioProcessor::SceneChainTransitionScope>(selectedId - 1));
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionScopeBox);
    styleSceneCombo(sceneTransitionScopeBox);

    sceneTransitionContourBox.addItem("Smooth", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::Smooth) + 1);
    sceneTransitionContourBox.addItem("Ramp", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::Ramp) + 1);
    sceneTransitionContourBox.addItem("Burst", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::BurstEnd) + 1);
    sceneTransitionContourBox.addItem("Duck/Lift", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::DuckThenLift) + 1);
    sceneTransitionContourBox.addItem("Late Hit", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::LateHit) + 1);
    sceneTransitionContourBox.setTooltip("Shape how the fill blooms over time without changing the core Chain Fill type.");
    sceneTransitionContourBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionContourBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        processor.setSceneChainStepTransitionContour(
            focusedStep,
            static_cast<MlrVSTAudioProcessor::SceneChainTransitionContour>(selectedId - 1));
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionContourBox);
    styleSceneCombo(sceneTransitionContourBox);

    sceneTransitionConditionBox.addItem("Always", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionCondition::Always) + 1);
    sceneTransitionConditionBox.addItem("50%", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionCondition::Chance50) + 1);
    sceneTransitionConditionBox.addItem("25%", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionCondition::Chance25) + 1);
    sceneTransitionConditionBox.addItem("Loop", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionCondition::LoopOnly) + 1);
    sceneTransitionConditionBox.addItem("Forward", static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionCondition::ForwardOnly) + 1);
    sceneTransitionConditionBox.setTooltip("Decide when the focused Chain Fill is allowed to fire.");
    sceneTransitionConditionBox.onChange = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        const int selectedId = sceneTransitionConditionBox.getSelectedId();
        if (focusedStep < 0 || selectedId <= 0)
            return;

        processor.setSceneChainStepTransitionCondition(
            focusedStep,
            static_cast<MlrVSTAudioProcessor::SceneChainTransitionCondition>(selectedId - 1));
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionConditionBox);
    styleSceneCombo(sceneTransitionConditionBox);

    sceneTransitionEndSampleBox.onChange = [this]()
    {
        if (sceneTransitionEndSampleBoxUpdating)
            return;

        const int focusedStep = getFocusedSceneChainStep() >= 0
            ? getFocusedSceneChainStep()
            : (sceneEndSampleEditorVisible ? sceneEndSampleEditorStep : -1);
        if (focusedStep < 0)
            return;

        juce::File selectedFile;
        const int selectedId = sceneTransitionEndSampleBox.getSelectedId();
        if (selectedId >= 2)
        {
            const int choiceIndex = selectedId - 2;
            if (choiceIndex >= 0 && choiceIndex < static_cast<int>(sceneTransitionEndSampleChoices.size()))
                selectedFile = sceneTransitionEndSampleChoices[static_cast<size_t>(choiceIndex)];
        }

        if (selectedFile != juce::File())
        {
            sceneTransitionEndSampleListedDirectory = selectedFile.getParentDirectory();
            processor.setSceneTransitionEndSampleDirectory(selectedFile.getParentDirectory());
        }

        processor.setSceneChainStepTransitionEndSampleFile(focusedStep, selectedFile);
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionEndSampleBox);
    styleSceneCombo(sceneTransitionEndSampleBox);

    sceneTransitionEndSampleFolderButton.setButtonText("Edit");
    sceneTransitionEndSampleFolderButton.setTooltip("Open the fill-end sample editor for the focused bubble.");
    sceneTransitionEndSampleFolderButton.onClick = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep() >= 0
            ? getFocusedSceneChainStep()
            : (sceneEndSampleEditorVisible ? sceneEndSampleEditorStep : -1);
        if (focusedStep < 0)
            return;

        if (sceneEndSampleEditorVisible && sceneEndSampleEditorStep == focusedStep)
        {
            closeSceneTransitionEndSampleEditor();
            return;
        }

        openSceneTransitionEndSampleEditor(focusedStep);
    };
    addAndMakeVisible(sceneTransitionEndSampleFolderButton);
    styleSceneUtilityButton(sceneTransitionEndSampleFolderButton);

    auto configureAmountSlider = [&](juce::Slider& slider,
                                     float defaultValue,
                                     const juce::String& suffix,
                                     bool primaryMacroControl,
                                     auto&& onCommit)
    {
        juce::ignoreUnused(primaryMacroControl);
        slider.setRange(0.0, 1.0, 0.01);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.textFromValueFunction = [suffix](double value)
        {
            return juce::String(juce::roundToInt(value * 100.0)) + suffix;
        };
        slider.onValueChange = [this, &slider, onCommit]()
        {
            const int focusedStep = getFocusedSceneChainStep();
            if (focusedStep < 0)
                return;

            if (&slider == &sceneTransitionMixSlider
                && sceneTransitionSmartLinkEnabled
                && !sceneTransitionApplyingLinkedTuning)
            {
                const auto currentType = processor.getSceneChainStepTransitionType(focusedStep);
                if (currentType != MlrVSTAudioProcessor::SceneChainTransitionType::None
                    && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
                {
                    applySceneTransitionSmartTuning(focusedStep,
                                                    currentType,
                                                    processor.getSceneChainStepTransitionOption(focusedStep),
                                                    static_cast<float>(slider.getValue()),
                                                    processor.getSceneChainStepTransitionLengthBeats(focusedStep),
                                                    processor.getSceneChainStepTransitionSubtractsFromSceneLength(focusedStep),
                                                    false,
                                                    false);
                    return;
                }
            }

            onCommit(focusedStep, static_cast<float>(slider.getValue()));
            refreshFromProcessor();
        };
        addAndMakeVisible(slider);
        styleSceneTransitionSlider(slider);
    };

    sceneTransitionMixSlider.setTooltip("Overall fill intensity. Higher values push the chosen fill type harder.");
    configureAmountSlider(sceneTransitionMixSlider,
                          MlrVSTAudioProcessor::DefaultSceneTransitionIntensity,
                          "%",
                          true,
                          [this](int focusedStep, float value)
                          {
                              processor.setSceneChainStepTransitionIntensity(focusedStep, value);
                          });

    sceneTransitionDelaySlider.setTooltip("Extra delay send used by the focused fill bubble.");
    configureAmountSlider(sceneTransitionDelaySlider,
                          MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount,
                          "%",
                          false,
                          [this](int focusedStep, float value)
                          {
                              processor.setSceneChainStepTransitionDelayAmount(focusedStep, value);
                          });

    sceneTransitionFilterSlider.setTooltip("Filter movement used by the focused fill bubble.");
    configureAmountSlider(sceneTransitionFilterSlider,
                          MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount,
                          "%",
                          false,
                          [this](int focusedStep, float value)
                          {
                              processor.setSceneChainStepTransitionFilterAmount(focusedStep, value);
                          });

    sceneTransitionChopSlider.setTooltip("Gate/stutter amount used by the focused fill bubble.");
    configureAmountSlider(sceneTransitionChopSlider,
                          MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount,
                          "%",
                          false,
                          [this](int focusedStep, float value)
                          {
                              processor.setSceneChainStepTransitionChopAmount(focusedStep, value);
                          });

    sceneTransitionSubtractButton.setButtonText(sceneTransitionSubtractButtonLabel(false));
    sceneTransitionSubtractButton.setClickingTogglesState(true);
    sceneTransitionSubtractButton.setTooltip("When on, the fill lead time is subtracted from the outgoing scene duration. A 2-beat fill on an 8-beat scene will hand off after 6 beats.");
    sceneTransitionSubtractButton.onClick = [this]()
    {
        const int focusedStep = getFocusedSceneChainStep();
        if (focusedStep < 0)
            return;

        processor.setSceneChainStepTransitionSubtractsFromSceneLength(
            focusedStep,
            sceneTransitionSubtractButton.getToggleState());
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneTransitionSubtractButton);
    styleSceneToolButton(sceneTransitionSubtractButton, true);

    sceneTransitionSmartLinkButton.setButtonText("Linked");
    sceneTransitionSmartLinkButton.setClickingTogglesState(true);
    sceneTransitionSmartLinkButton.setToggleState(sceneTransitionSmartLinkEnabled, juce::dontSendNotification);
    sceneTransitionSmartLinkButton.setTooltip("When Linked is on, Type, Style, Lead, and Blend automatically tune Echo, Tone, and Gate for the focused Chain Fill. Touch Echo, Tone, or Gate directly to switch back to manual editing.");
    sceneTransitionSmartLinkButton.onClick = [this]()
    {
        sceneTransitionSmartLinkEnabled = sceneTransitionSmartLinkButton.getToggleState();
        refreshFromProcessor();
    };
    styleSceneToolButton(sceneTransitionSmartLinkButton, true);

    auto configureTransitionQuickAction = [this, &styleSceneUtilityButton](juce::TextButton& button,
                                                                           const juce::String& text,
                                                                           const juce::String& tooltip,
                                                                           MlrVSTAudioProcessor::SceneChainTransitionType type,
                                                                           MlrVSTAudioProcessor::SceneChainTransitionOption option,
                                                                           float intensity,
                                                                           float lengthBeats,
                                                                           bool subtractFromSceneLength)
    {
        button.setButtonText(text);
        button.setTooltip(tooltip);
        button.onClick = [this, type, option, intensity, lengthBeats, subtractFromSceneLength]()
        {
            const int focusedStep = getFocusedSceneChainStep();
            if (focusedStep < 0)
                return;

            applySceneTransitionQuickAction(focusedStep,
                                            type,
                                            option,
                                            intensity,
                                            lengthBeats,
                                            subtractFromSceneLength);
        };
        styleSceneUtilityButton(button);
    };

    configureTransitionQuickAction(sceneTransitionPunchButton,
                                   "Punch",
                                   "Quick Chain Fill: tight stutter with a short lead and punchy gate.",
                                   MlrVSTAudioProcessor::SceneChainTransitionType::Stutter,
                                   MlrVSTAudioProcessor::SceneChainTransitionOption::Tight,
                                   0.82f,
                                   0.5f,
                                   true);
    configureTransitionQuickAction(sceneTransitionLiftButton,
                                   "Lift",
                                   "Quick Chain Fill: rising filter sweep with a longer musical lift into the next scene.",
                                   MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise,
                                   MlrVSTAudioProcessor::SceneChainTransitionOption::Sweep,
                                   0.80f,
                                   2.0f,
                                   false);
    configureTransitionQuickAction(sceneTransitionEchoButton,
                                   "Echo",
                                   "Quick Chain Fill: delay-led handoff with a little movement and tail.",
                                   MlrVSTAudioProcessor::SceneChainTransitionType::Fill,
                                   MlrVSTAudioProcessor::SceneChainTransitionOption::Echo,
                                   0.76f,
                                   1.0f,
                                   false);
    configureTransitionQuickAction(sceneTransitionDropButton,
                                   "Drop",
                                   "Quick Chain Fill: short drop into the next scene with stronger cut and gate.",
                                   MlrVSTAudioProcessor::SceneChainTransitionType::Drop,
                                   MlrVSTAudioProcessor::SceneChainTransitionOption::Gate,
                                   0.86f,
                                   1.0f,
                                   true);

    addAndMakeVisible(sceneChainCanvas);

    sceneRecorderTitleLabel.setText("CLIP", juce::dontSendNotification);
    sceneRecorderTitleLabel.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    sceneRecorderTitleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    sceneRecorderTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneRecorderTitleLabel);
    sceneRecorderTitleLabel.setVisible(false);

    sceneRecordButton.setButtonText("Rec");
    sceneRecordButton.setClickingTogglesState(false);
    sceneRecordButton.setSingleTriggerImmediate(true);
    sceneRecordButton.setTooltip("Single-click arms recording for the next scene-change boundary and captures one scene-length pass. Double-click overdubs another pass. Monome scene-row x7 uses the same record gesture.");
    sceneRecordButton.onSingleTrigger = [this]()
    {
        if (processor.isScenePerformanceRecording())
            processor.stopScenePerformanceRecording();
        else
        {
            auto currentEvents = processor.getScenePerformanceEventsSnapshot(getFocusedSceneSlot());
            ensureSceneDefaultAutomationStartPoints(processor, currentEvents);
            processor.replaceScenePerformanceClipEvents(getFocusedSceneSlot(), currentEvents);
            processor.startScenePerformanceRecording(false);
        }
        refreshFromProcessor();
    };
    sceneRecordButton.onDoubleTrigger = [this]()
    {
        if (processor.isScenePerformanceRecording() && processor.isScenePerformanceOverdubbing())
            processor.extendScenePerformanceRecording();
        else
            processor.startScenePerformanceRecording(true);
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneRecordButton);
    styleScenePrimaryButton(sceneRecordButton, "transport");

    sceneClearButton.setButtonText("Clear");
    sceneClearButton.setTooltip("Erase the focused scene clip and reset its scene motion lanes.");
    sceneClearButton.onClick = [this]()
    {
        const int sceneSlot = getFocusedSceneSlot();
        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            processor.clearSceneMotionStripState(sceneSlot, stripIndex);
        processor.clearScenePerformanceClip(sceneSlot);
        clearSceneSelection();
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneClearButton);
    styleSceneUtilityButton(sceneClearButton);
    sceneClearButton.setVisible(false);

    sceneDeleteButton.setButtonText("Delete");
    sceneDeleteButton.setTooltip("Delete the selected scene event.");
    sceneDeleteButton.onClick = [this]() { deleteSelectedSceneEvent(); };
    addAndMakeVisible(sceneDeleteButton);
    styleSceneUtilityButton(sceneDeleteButton);
    sceneDeleteButton.setVisible(false);

    sceneClearTriggersButton.setButtonText("Trig");
    sceneClearTriggersButton.setTooltip("Remove all trigger events from the scene clip.");
    sceneClearTriggersButton.onClick = [this]() { clearSceneEventsByType(ScenePerformanceEventType::Trigger); };
    addAndMakeVisible(sceneClearTriggersButton);
    styleSceneUtilityButton(sceneClearTriggersButton);
    sceneClearTriggersButton.setVisible(false);

    sceneClearControlsButton.setButtonText("Ctrl");
    sceneClearControlsButton.setTooltip("Remove all recorded control points from the scene clip.");
    sceneClearControlsButton.onClick = [this]() { clearSceneEventsByType(ScenePerformanceEventType::ControlPoint); };
    addAndMakeVisible(sceneClearControlsButton);
    styleSceneUtilityButton(sceneClearControlsButton);
    sceneClearControlsButton.setVisible(false);

    sceneGridToggleButton.setButtonText("Snap");
    sceneGridToggleButton.setClickingTogglesState(true);
    sceneGridToggleButton.setToggleState(sceneGridEnabled, juce::dontSendNotification);
    sceneGridToggleButton.setTooltip("Snap clip edits to the grid and show the grid overlay.");
    sceneGridToggleButton.onClick = [this]()
    {
        sceneGridEnabled = sceneGridToggleButton.getToggleState();
        processor.setSceneEditorGridEnabled(sceneGridEnabled);
        sceneGridDivisionBox.setEnabled(sceneGridEnabled);
        sceneTimelineCanvas.repaint();
    };
    addAndMakeVisible(sceneGridToggleButton);
    styleSceneToolButton(sceneGridToggleButton);

    sceneGridDivisionBox.addItem("1/4", 4);
    sceneGridDivisionBox.addItem("1/8", 8);
    sceneGridDivisionBox.addItem("1/16", 16);
    sceneGridDivisionBox.addItem("1/32", 32);
    sceneGridDivisionBox.setSelectedId(sceneGridDivision, juce::dontSendNotification);
    sceneGridDivisionBox.setTooltip("Snap and grid resolution for the clip editor.");
    sceneGridDivisionBox.onChange = [this]()
    {
        const int selectedId = sceneGridDivisionBox.getSelectedId();
        if (selectedId > 0)
        {
            sceneGridDivision = selectedId;
            processor.setSceneEditorGridDivision(sceneGridDivision);
            sceneTimelineCanvas.repaint();
        }
    };
    addAndMakeVisible(sceneGridDivisionBox);
    styleSceneCombo(sceneGridDivisionBox);

    for (int factor : {1, 2, 3, 4, 5, 6, 8})
        sceneZoomBox.addItem(juce::String(factor) + "x", factor);
    sceneZoomBox.setSelectedId(sceneZoomFactor, juce::dontSendNotification);
    sceneZoomBox.setTooltip("Horizontal zoom for the clip editor.");
    sceneZoomBox.onChange = [this]()
    {
        const int selectedId = sceneZoomBox.getSelectedId();
        if (selectedId > 0)
        {
            sceneZoomFactor = selectedId;
            processor.setSceneEditorZoomFactor(sceneZoomFactor);
            updateSceneTimelineContentSize();
            updateSceneViewportFollow();
            sceneTimelineCanvas.repaint();
        }
    };
    addAndMakeVisible(sceneZoomBox);
    styleSceneCombo(sceneZoomBox);

    sceneFollowButton.setButtonText("Follow");
    sceneFollowButton.setClickingTogglesState(true);
    sceneFollowButton.setToggleState(sceneFollowPlayheadEnabled, juce::dontSendNotification);
    sceneFollowButton.setTooltip("Keep the live scene playhead centered in view while the scene is running.");
    sceneFollowButton.onClick = [this]()
    {
        sceneFollowPlayheadEnabled = sceneFollowButton.getToggleState();
        processor.setSceneEditorFollowPlayheadEnabled(sceneFollowPlayheadEnabled);
        if (sceneFollowPlayheadEnabled)
            updateSceneViewportFollow();
    };
    addAndMakeVisible(sceneFollowButton);
    styleSceneToolButton(sceneFollowButton);

    sceneReenableAutomationButton.setButtonText("Re-enable");
    sceneReenableAutomationButton.setTooltip("If a scene automation lane is greyed out, a live touch has overridden it. Click to re-enable the written scene automation. Step motion lanes are unaffected.");
    sceneReenableAutomationButton.onClick = [this]()
    {
        processor.reenableActiveSceneAutomation();
        refreshFromProcessor();
        const double beat = processor.getAudioEngine() != nullptr
            ? processor.getAudioEngine()->getTimelineBeat()
            : 0.0;
        updateSceneEditorState(beat);
        sceneTimelineCanvas.repaint();
    };
    addAndMakeVisible(sceneReenableAutomationButton);
    styleSceneToolButton(sceneReenableAutomationButton);

    sceneLaneOverlayButton.setButtonText("Tint");
    sceneLaneOverlayButton.setClickingTogglesState(true);
    sceneLaneOverlayButton.setToggleState(sceneLaneOverlayEnabled, juce::dontSendNotification);
    sceneLaneOverlayButton.setTooltip("Show or hide the colored lane overlays in the scene editor.");
    sceneLaneOverlayButton.onClick = [this]()
    {
        sceneLaneOverlayEnabled = sceneLaneOverlayButton.getToggleState();
        processor.setSceneEditorLaneOverlaysEnabled(sceneLaneOverlayEnabled);
        sceneTimelineCanvas.repaint();
    };
    addAndMakeVisible(sceneLaneOverlayButton);
    styleSceneToolButton(sceneLaneOverlayButton);

    sceneModPageLabel.setText("Mod Page", juce::dontSendNotification);
    sceneModPageLabel.setJustificationType(juce::Justification::centredLeft);
    sceneModPageLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(sceneModPageLabel);

    styleSceneCombo(sceneModPageBox);
    sceneModPageBox.addItem("Step Motion", static_cast<int>(MlrVSTAudioProcessor::SceneModPageMode::StepMotion) + 1);
    sceneModPageBox.addItem("Main Mod", static_cast<int>(MlrVSTAudioProcessor::SceneModPageMode::MainModulation) + 1);
    sceneModPageBox.onChange = [this]()
    {
        const int selectedId = sceneModPageBox.getSelectedId();
        if (selectedId <= 0)
            return;

        processor.setSceneModPageMode(static_cast<MlrVSTAudioProcessor::SceneModPageMode>(selectedId - 1));
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneModPageBox);

    sceneMotionEditButton.setButtonText("Mod");
    sceneMotionEditButton.setTooltip("Open the classic mod-step editor for the hovered or selected scene motion lane. Depth, Clock, Shape, and Length live inside the embedded motion editor.");
    sceneMotionEditButton.onClick = [this]()
    {
        if (sceneLegacyModEditorVisible)
        {
            closeLegacyModEditor();
            return;
        }

        const int fallbackStripIndex = juce::jlimit(0,
                                                    juce::jmax(0, getVisibleSceneStripCount() - 1),
                                                    sceneEditorState.selectedStripIndex);
        const int stripIndex = sceneEditorState.hoverStripIndex >= 0 ? sceneEditorState.hoverStripIndex : fallbackStripIndex;
        const int laneIndex = sceneEditorState.hoverLaneIndex >= 0 ? sceneEditorState.hoverLaneIndex : 0;
        openLegacyModEditorForLane(stripIndex, laneIndex);
    };
    sceneMotionEditButton.setVisible(false);

    sceneDuplicateButton.setButtonText("Dup");
    sceneDuplicateButton.setTooltip("Duplicate the selected scene events one grid step later. Shortcut: Cmd/Ctrl+D.");
    sceneDuplicateButton.onClick = [this]() { duplicateSelectedSceneEvents(); };
    addAndMakeVisible(sceneDuplicateButton);
    styleSceneUtilityButton(sceneDuplicateButton);

    sceneNudgeLeftButton.setButtonText("N<");
    sceneNudgeLeftButton.setTooltip("Nudge the selected scene events left by one grid step.");
    sceneNudgeLeftButton.onClick = [this]() { nudgeSelectedSceneEvents(-1); };
    addAndMakeVisible(sceneNudgeLeftButton);
    styleSceneUtilityButton(sceneNudgeLeftButton);

    sceneNudgeRightButton.setButtonText("N>");
    sceneNudgeRightButton.setTooltip("Nudge the selected scene events right by one grid step.");
    sceneNudgeRightButton.onClick = [this]() { nudgeSelectedSceneEvents(1); };
    addAndMakeVisible(sceneNudgeRightButton);
    styleSceneUtilityButton(sceneNudgeRightButton);

    sceneQuantizeButton.setButtonText("Qtz");
    sceneQuantizeButton.setTooltip("Quantize the selected scene events to the current grid.");
    sceneQuantizeButton.onClick = [this]() { quantizeSelectedSceneEvents(); };
    addAndMakeVisible(sceneQuantizeButton);
    styleSceneUtilityButton(sceneQuantizeButton);

    sceneExpandAllLanesButton.setButtonText("All+");
    sceneExpandAllLanesButton.setTooltip("Expand the automation lanes for all strips.");
    sceneExpandAllLanesButton.onClick = [this]()
    {
        stripAutomationExpanded.fill(true);
        processor.setSceneEditorStripAutomationExpandedAll(true);
        updateSceneTimelineContentSize();
        sceneTimelineCanvas.repaint();
    };
    addAndMakeVisible(sceneExpandAllLanesButton);
    styleSceneUtilityButton(sceneExpandAllLanesButton);

    sceneCollapseAllLanesButton.setButtonText("All-");
    sceneCollapseAllLanesButton.setTooltip("Collapse the automation lanes for all strips.");
    sceneCollapseAllLanesButton.onClick = [this]()
    {
        stripAutomationExpanded.fill(false);
        processor.setSceneEditorStripAutomationExpandedAll(false);
        updateSceneTimelineContentSize();
        sceneTimelineCanvas.repaint();
    };
    addAndMakeVisible(sceneCollapseAllLanesButton);
    styleSceneUtilityButton(sceneCollapseAllLanesButton);

    sceneStatusLabel.setText("EMPTY", juce::dontSendNotification);
    sceneStatusLabel.setFont(juce::Font(juce::FontOptions(9.8f, juce::Font::bold)));
    sceneStatusLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff1b2125));
    sceneStatusLabel.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.12f));
    sceneStatusLabel.setBorderSize({2, 8, 2, 8});
    sceneStatusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(sceneStatusLabel);

    sceneDetailLabel.setFont(juce::Font(juce::FontOptions(9.8f)));
    sceneDetailLabel.setColour(juce::Label::textColourId, kTextSecondary);
    sceneDetailLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneDetailLabel);

    sceneSelectionLabel.setFont(juce::Font(juce::FontOptions(9.1f)));
    sceneSelectionLabel.setColour(juce::Label::textColourId, kTextSecondary.withAlpha(0.92f));
    sceneSelectionLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneSelectionLabel);
    sceneSelectionLabel.setVisible(false);

    sceneViewport.setViewedComponent(&sceneTimelineCanvas, false);
    sceneViewport.setScrollBarsShown(true, true, true, true);
    sceneViewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible(sceneViewport);

    sceneLegacyModBackdrop.setVisible(false);
    sceneLegacyModBackdrop.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(sceneLegacyModBackdrop);

    sceneLegacyModEditor = std::make_unique<ModulationControlPanel>(processor);
    sceneLegacyModEditor->onPinnedSceneMotionChange = [this]() { refreshFromProcessor(); };
    sceneLegacyModEditor->onSceneStripWrite = [this](int stripIndex)
    {
        writeCurrentStripAutomation(stripIndex);
        refreshFromProcessor();
    };
    sceneLegacyModEditor->onSceneStripWriteAll = [this]()
    {
        writeAllStripsAutomation();
        refreshFromProcessor();
    };
    sceneLegacyModEditor->onSceneStripClear = [this](int stripIndex)
    {
        clearStripSceneEvents(stripIndex);
        refreshFromProcessor();
    };
    sceneLegacyModEditor->onSceneStripDuplicate = [this](int stripIndex)
    {
        duplicateStripSceneEventsToNext(stripIndex);
        refreshFromProcessor();
    };
    sceneLegacyModEditor->onSceneStripCopyTo = [this](int sourceStrip, int destStrip)
    {
        copyStripSceneEvents(sourceStrip, destStrip);
        refreshFromProcessor();
    };
    sceneLegacyModEditor->onMouseWheelPassthrough = [this](const juce::MouseEvent& e,
                                                           const juce::MouseWheelDetails& wheel)
    {
        handleSceneTimelineMouseWheel(e, wheel);
    };
    addAndMakeVisible(*sceneLegacyModEditor);
    sceneLegacyModEditor->setVisible(false);
    sceneLegacyModEditor->addAndMakeVisible(sceneLaneOverlayButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneDuplicateButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneNudgeLeftButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneNudgeRightButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneQuantizeButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneExpandAllLanesButton);
    sceneLegacyModEditor->addAndMakeVisible(sceneCollapseAllLanesButton);

    sceneLegacyModCloseButton.setButtonText("Close");
    sceneLegacyModCloseButton.setTooltip("Close the embedded scene motion editor.");
    sceneLegacyModCloseButton.onClick = [this]()
    {
        if (processor.isSceneModeEnabled()
            && processor.isControlModeActive()
            && processor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation)
        {
            processor.setControlModeFromGui(MlrVSTAudioProcessor::ControlMode::Normal, false);
        }
        closeLegacyModEditor();
        refreshFromProcessor();
    };
    addAndMakeVisible(sceneLegacyModCloseButton);
    styleSceneUtilityButton(sceneLegacyModCloseButton);
    sceneLegacyModCloseButton.setVisible(false);

    sceneEndSampleEditorBackdrop.setVisible(false);
    sceneEndSampleEditorBackdrop.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(sceneEndSampleEditorBackdrop);

    sceneEndSampleEditor = std::make_unique<SceneTransitionEndSampleEditorPanel>(processor);
    sceneEndSampleEditor->onChanged = [this]()
    {
        sceneTransitionEndSampleListedDirectory = processor.getSceneTransitionEndSampleDirectory();
        rebuildSceneTransitionEndSampleChoices();
        refreshFromProcessor();
    };
    sceneEndSampleEditor->onCloseRequested = [this]() { closeSceneTransitionEndSampleEditor(); };
    addAndMakeVisible(*sceneEndSampleEditor);
    sceneEndSampleEditor->setVisible(false);

    for (int stripIndex = 0; stripIndex < SceneEditorVisibleStrips; ++stripIndex)
    {
        for (int lane = 0; lane < SceneAutomationLaneCount; ++lane)
        {
            auto& targetBox = sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)];
            targetBox.addItem("Unassigned", performanceTargetToComboId(ModernAudioEngine::ModTarget::None));
            for (auto target : kModPerformanceTargetOrder)
            {
                targetBox.addItem(performanceTargetDisplayName(target, true),
                                  performanceTargetToComboId(target));
            }
            targetBox.setTooltip("Choose which motion target this strip lane should own. "
                                 "Double-click the strip label and zoom in to show these selectors.");
            targetBox.onChange = [this, stripIndex, lane]()
            {
                const int selectedId =
                    sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)].getSelectedId();
                if (selectedId <= 0)
                    return;

                const auto target = sanitizeModPerformanceTarget(performanceTargetFromComboId(selectedId));
                auto& mutableProcessor = processor;
                const int assignedSlot = sceneAssignedModSlotForLane(mutableProcessor, stripIndex, lane);
                const int targetSlot = assignedSlot >= 0
                    ? assignedSlot
                    : preferredLegacyModEditorSlotForLane(stripIndex, lane);
                if (target == ModernAudioEngine::ModTarget::None && assignedSlot < 0)
                    return;
                if (targetSlot < 0)
                    return;

                processor.setSceneMotionTargetForSlot(
                    stripIndex,
                    targetSlot,
                    target);
                refreshFromProcessor();
            };
            styleSceneCombo(targetBox);
            sceneTimelineCanvas.addAndMakeVisible(targetBox);
        }
    }

    refreshFromProcessor();
}

SceneControlPanel::~SceneControlPanel()
{
    processor.setSceneStepMotionEditorOpen(false);
}

void SceneControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);

    auto drawInsetSection = [&g](juce::Rectangle<int> bounds, juce::Colour accent)
    {
        if (bounds.isEmpty())
            return;

        auto sectionBounds = bounds.toFloat().expanded(6.0f, 6.0f);
        juce::ColourGradient sectionFill(juce::Colour(0xff161c21).withAlpha(0.92f),
                                         sectionBounds.getX(),
                                         sectionBounds.getY(),
                                         juce::Colour(0xff101419).withAlpha(0.96f),
                                         sectionBounds.getX(),
                                         sectionBounds.getBottom(),
                                         false);
        g.setGradientFill(sectionFill);
        g.fillRoundedRectangle(sectionBounds, 10.0f);

        const auto topSheen = sectionBounds.reduced(1.0f).removeFromTop(sectionBounds.getHeight() * 0.42f);
        g.setColour(juce::Colours::white.withAlpha(0.035f));
        g.fillRoundedRectangle(topSheen, 8.8f);

        g.setColour(juce::Colours::white.withAlpha(0.045f));
        g.drawRoundedRectangle(sectionBounds.reduced(0.5f), 10.0f, 1.0f);

        g.setColour(accent.withAlpha(0.20f));
        g.drawRoundedRectangle(sectionBounds.reduced(1.5f), 8.8f, 1.0f);

        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.drawRoundedRectangle(sectionBounds.reduced(2.4f), 7.6f, 1.0f);
    };

    auto unionVisibleBounds = [](juce::Rectangle<int>& target, const juce::Component& component)
    {
        if (!component.isVisible())
            return;

        target = target.isEmpty() ? component.getBounds() : target.getUnion(component.getBounds());
    };

    juce::Rectangle<int> sceneBounds;
    unionVisibleBounds(sceneBounds, sceneSlotsSectionLabel);
    for (const auto& button : sceneSelectorButtons)
        unionVisibleBounds(sceneBounds, button);
    drawInsetSection(sceneBounds, sceneSlotUiColour(processor, getFocusedSceneSlot()));

    juce::Rectangle<int> playbackBounds;
    unionVisibleBounds(playbackBounds, scenePlaybackSectionLabel);
    unionVisibleBounds(playbackBounds, sceneChainPlayButton);
    unionVisibleBounds(playbackBounds, sceneChangeModeLabel);
    unionVisibleBounds(playbackBounds, sceneChangeModeBox);
    unionVisibleBounds(playbackBounds, sceneLengthHeaderLabel);
    unionVisibleBounds(playbackBounds, sceneLengthModeBoxes[static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, getFocusedSceneSlot()))]);
    unionVisibleBounds(playbackBounds, sceneBarsHeaderLabel);
    unionVisibleBounds(playbackBounds, sceneManualBarsBoxes[static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, getFocusedSceneSlot()))]);
    unionVisibleBounds(playbackBounds, sceneAnchorHeaderLabel);
    unionVisibleBounds(playbackBounds, sceneAnchorStripBoxes[static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, getFocusedSceneSlot()))]);
    drawInsetSection(playbackBounds, kAccent);

    juce::Rectangle<int> actionBounds;
    unionVisibleBounds(actionBounds, sceneInsertBeforeButton);
    unionVisibleBounds(actionBounds, sceneInsertAfterButton);
    unionVisibleBounds(actionBounds, sceneSceneCopyButton);
    unionVisibleBounds(actionBounds, sceneScenePasteButton);
    unionVisibleBounds(actionBounds, sceneDuplicateLengthButton);
    unionVisibleBounds(actionBounds, sceneChainClearButton);
    drawInsetSection(actionBounds, juce::Colour(0xff89949d));

    juce::Rectangle<int> chainBounds;
    unionVisibleBounds(chainBounds, sceneChainCanvas);
    drawInsetSection(chainBounds, sceneSlotUiColour(processor, getFocusedSceneSlot()).withAlpha(0.94f));

    juce::Rectangle<int> fillBounds;
    unionVisibleBounds(fillBounds, sceneTransitionHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionTypeBox);
    unionVisibleBounds(fillBounds, sceneTransitionOptionsHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionOptionsBox);
    unionVisibleBounds(fillBounds, sceneTransitionLengthHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionLengthBox);
    unionVisibleBounds(fillBounds, sceneTransitionSubtractButton);
    unionVisibleBounds(fillBounds, sceneTransitionMixHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionMixSlider);
    unionVisibleBounds(fillBounds, sceneTransitionDelayHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionDelaySlider);
    unionVisibleBounds(fillBounds, sceneTransitionFilterHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionFilterSlider);
    unionVisibleBounds(fillBounds, sceneTransitionChopHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionChopSlider);
    unionVisibleBounds(fillBounds, sceneTransitionScopeHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionScopeBox);
    unionVisibleBounds(fillBounds, sceneTransitionContourHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionContourBox);
    unionVisibleBounds(fillBounds, sceneTransitionConditionHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionConditionBox);
    unionVisibleBounds(fillBounds, sceneTransitionEndSampleHeaderLabel);
    unionVisibleBounds(fillBounds, sceneTransitionEndSampleBox);
    unionVisibleBounds(fillBounds, sceneTransitionEndSampleFolderButton);
    unionVisibleBounds(fillBounds, sceneTransitionSmartLinkButton);
    unionVisibleBounds(fillBounds, sceneTransitionPunchButton);
    unionVisibleBounds(fillBounds, sceneTransitionLiftButton);
    unionVisibleBounds(fillBounds, sceneTransitionEchoButton);
    unionVisibleBounds(fillBounds, sceneTransitionDropButton);
    unionVisibleBounds(fillBounds, sceneTransitionSummaryLabel);
    unionVisibleBounds(fillBounds, sceneTransitionMetaLabel);
    drawInsetSection(fillBounds, sceneSlotUiColour(processor, getFocusedSceneSlot()).brighter(0.12f));

    juce::Rectangle<int> timelineBounds;
    unionVisibleBounds(timelineBounds, sceneViewport);
    unionVisibleBounds(timelineBounds, sceneRecorderTitleLabel);
    unionVisibleBounds(timelineBounds, sceneGridToggleButton);
    unionVisibleBounds(timelineBounds, sceneGridDivisionBox);
    unionVisibleBounds(timelineBounds, sceneZoomBox);
    unionVisibleBounds(timelineBounds, sceneFollowButton);
    unionVisibleBounds(timelineBounds, sceneReenableAutomationButton);
    unionVisibleBounds(timelineBounds, sceneRecordButton);
    unionVisibleBounds(timelineBounds, sceneCaptureButton);
    unionVisibleBounds(timelineBounds, sceneStatusLabel);
    unionVisibleBounds(timelineBounds, sceneDetailLabel);
    unionVisibleBounds(timelineBounds, sceneSelectionLabel);
    drawInsetSection(timelineBounds, sceneStatusLabel.findColour(juce::Label::textColourId));
}

void SceneControlPanel::visibilityChanged()
{
    if (isShowing())
        refreshFromProcessor();
}

juce::Rectangle<int> SceneControlPanel::getSceneViewportBounds() const
{
    return sceneViewport.getBounds();
}

bool SceneControlPanel::shouldShowSceneMotionTargetSelectors(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= getVisibleSceneStripCount())
        return false;

    const auto index = static_cast<size_t>(stripIndex);
    return !sceneLegacyModEditorVisible
        && stripAutomationExpanded[index]
        && stripHeightExpanded[index];
}

int SceneControlPanel::getFocusedSceneSlot() const
{
    return processor.getFocusedSceneSlot();
}

int SceneControlPanel::getFocusedSceneChainStep() const
{
    const int chainLength = processor.getSceneChainLength();
    if (chainLength <= 0)
        return -1;

    if (selectedSceneChainStep >= 0 && selectedSceneChainStep < chainLength)
        return selectedSceneChainStep;

    const int activeStep = processor.isSceneChainPlaybackActive() ? processor.getSceneChainPlaybackStepIndex() : -1;
    if (activeStep >= 0 && activeStep < chainLength)
        return activeStep;

    return -1;
}

void SceneControlPanel::applySceneTransitionSmartTuning(int stepIndex,
                                                        MlrVSTAudioProcessor::SceneChainTransitionType type,
                                                        MlrVSTAudioProcessor::SceneChainTransitionOption option,
                                                        float intensity,
                                                        float lengthBeats,
                                                        bool subtractFromSceneLength,
                                                        bool updateTypeAndOption,
                                                        bool updateLength)
{
    if (stepIndex < 0 || stepIndex >= processor.getSceneChainLength())
        return;

    const auto tuned = sceneTransitionSuggestedSettings(type,
                                                        option,
                                                        intensity,
                                                        lengthBeats,
                                                        subtractFromSceneLength);
    const juce::ScopedValueSetter<bool> tuningGuard(sceneTransitionApplyingLinkedTuning, true);

    if (updateTypeAndOption)
    {
        processor.setSceneChainStepTransitionType(stepIndex, type);
        processor.setSceneChainStepTransitionOption(stepIndex, option);
    }

    if (updateLength)
        processor.setSceneChainStepTransitionLengthBeats(stepIndex, tuned.lengthBeats);

    processor.setSceneChainStepTransitionIntensity(stepIndex, tuned.intensity);
    processor.setSceneChainStepTransitionDelayAmount(stepIndex, tuned.delayAmount);
    processor.setSceneChainStepTransitionFilterAmount(stepIndex, tuned.filterAmount);
    processor.setSceneChainStepTransitionChopAmount(stepIndex, tuned.chopAmount);
    processor.setSceneChainStepTransitionSubtractsFromSceneLength(stepIndex, tuned.subtractFromSceneLength);
    refreshFromProcessor();
}

void SceneControlPanel::applySceneTransitionQuickAction(int stepIndex,
                                                        MlrVSTAudioProcessor::SceneChainTransitionType type,
                                                        MlrVSTAudioProcessor::SceneChainTransitionOption option,
                                                        float intensity,
                                                        float lengthBeats,
                                                        bool subtractFromSceneLength)
{
    sceneTransitionSmartLinkEnabled = true;
    sceneTransitionSmartLinkButton.setToggleState(true, juce::dontSendNotification);
    applySceneTransitionSmartTuning(stepIndex,
                                    type,
                                    option,
                                    intensity,
                                    lengthBeats,
                                    subtractFromSceneLength,
                                    true,
                                    true);
}

void SceneControlPanel::rebuildSceneTransitionEndSampleChoices()
{
    auto directory = sceneTransitionEndSampleListedDirectory;
    if (!sceneTransitionEndSampleDirectoryAvailable(directory))
        directory = effectiveSceneTransitionEndSampleDirectory(processor, getFocusedSceneChainStep());

    if (directory == sceneTransitionEndSampleListedDirectory)
        return;

    sceneTransitionEndSampleListedDirectory = directory;
    sceneTransitionEndSampleChoices = collectSceneTransitionEndSampleChoices(directory);
}

int SceneControlPanel::getVisibleSceneStripCount() const
{
    const int maxVisibleStrips = juce::jmin(SceneEditorVisibleStrips, MlrVSTAudioProcessor::MaxStrips);
    const int monomeWidth = processor.getMonomeGridWidth();
    const int monomeHeight = processor.getMonomeGridHeight();

    if (monomeWidth == 16 && monomeHeight == 8)
        return juce::jlimit(0, maxVisibleStrips, processor.getMonomeActiveStripCount());

    return maxVisibleStrips;
}

int SceneControlPanel::getSceneTimelineContentHeight() const
{
    float height = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int stripIndex = 0; stripIndex < getVisibleSceneStripCount(); ++stripIndex)
    {
        height += sceneStripCardHeight(processor,
                                       stripIndex,
                                       stripAutomationExpanded[static_cast<size_t>(stripIndex)],
                                       stripHeightExpanded[static_cast<size_t>(stripIndex)]);
        if (stripIndex < getVisibleSceneStripCount() - 1)
            height += kSceneCardGap;
    }

    return juce::jmax(sceneViewport.getHeight(), static_cast<int>(std::ceil(height + 2.0f)));
}

bool SceneControlPanel::isSceneEventIndexSelected(int eventIndex) const
{
    return std::find(sceneEditorState.selectedEventIndices.begin(),
                     sceneEditorState.selectedEventIndices.end(),
                     eventIndex) != sceneEditorState.selectedEventIndices.end();
}

void SceneControlPanel::setSceneSelectionIndices(std::vector<int> indices, int primaryIndex)
{
    auto syncSelectedStripFromPrimary = [this]()
    {
        if (sceneEditorState.selectedEventIndex < 0
            || sceneEditorState.selectedEventIndex >= static_cast<int>(sceneEditorState.events.size()))
        {
            return;
        }

        const auto& selectedEvent = sceneEditorState.events[static_cast<size_t>(sceneEditorState.selectedEventIndex)];
        if (selectedEvent.stripIndex >= 0)
        {
            sceneEditorState.selectedStripIndex = juce::jlimit(0,
                                                               juce::jmax(0, getVisibleSceneStripCount() - 1),
                                                               selectedEvent.stripIndex);
        }
    };

    indices.erase(std::remove_if(indices.begin(),
                                 indices.end(),
                                 [this](int index)
                                 {
                                     return index < 0 || index >= static_cast<int>(sceneEditorState.events.size());
                                 }),
                  indices.end());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    sceneEditorState.selectedEventIndices = std::move(indices);

    if (sceneEditorState.selectedEventIndices.empty())
    {
        sceneEditorState.selectedEventIndex = -1;
        return;
    }

    if (primaryIndex >= 0 && isSceneEventIndexSelected(primaryIndex))
    {
        sceneEditorState.selectedEventIndex = primaryIndex;
        syncSelectedStripFromPrimary();
        return;
    }

    if (sceneEditorState.selectedEventIndex >= 0 && isSceneEventIndexSelected(sceneEditorState.selectedEventIndex))
    {
        syncSelectedStripFromPrimary();
        return;
    }

    sceneEditorState.selectedEventIndex = sceneEditorState.selectedEventIndices.front();
    syncSelectedStripFromPrimary();
}

void SceneControlPanel::clearSceneSelection()
{
    sceneEditorState.selectedEventIndices.clear();
    sceneEditorState.selectedEventIndex = -1;
}

std::vector<ScenePerformanceEvent> SceneControlPanel::getSelectedSceneEvents() const
{
    std::vector<ScenePerformanceEvent> selectedEvents;
    selectedEvents.reserve(sceneEditorState.selectedEventIndices.size());
    for (const int index : sceneEditorState.selectedEventIndices)
    {
        if (index < 0 || index >= static_cast<int>(sceneEditorState.events.size()))
            continue;
        selectedEvents.push_back(sceneEditorState.events[static_cast<size_t>(index)]);
    }
    return selectedEvents;
}

std::vector<int> SceneControlPanel::collectSceneEventIndicesInMarquee(juce::Rectangle<float> selectionRect) const
{
    std::vector<int> hitIndices;
    if (selectionRect.isEmpty())
        return hitIndices;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
        0.0f,
        0.0f,
        static_cast<float>(sceneTimelineCanvas.getWidth()),
        sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
        sceneGlobalLaneExpanded);
    const int globalLane = sceneGlobalAutomationLaneIndex();

    if (globalLane >= 0)
    {
        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            const auto marker = sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats).expanded(2.0f, 2.0f);
            if (selectionRect.intersects(marker))
                hitIndices.push_back(eventIndex);
        }
    }

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }
        if (layout.compactOverview)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (event.stripIndex != visibleStrip)
                continue;

            juce::Rectangle<float> marker;
            if (event.type == ScenePerformanceEventType::Trigger)
            {
                marker = sceneTriggerInteractiveBounds(layout, event, lengthBeats).expanded(2.0f, 2.0f);
            }
            else
            {
                if (!layout.automationExpanded)
                    continue;
                marker = sceneControlMarkerBounds(layout, event, lengthBeats).expanded(2.0f, 2.0f);
            }

            if (selectionRect.intersects(marker))
                hitIndices.push_back(eventIndex);
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    std::sort(hitIndices.begin(), hitIndices.end());
    hitIndices.erase(std::unique(hitIndices.begin(), hitIndices.end()), hitIndices.end());
    return hitIndices;
}

void SceneControlPanel::updateSceneHoverState(const juce::Point<float>& position, bool snapToGrid)
{
    bool hoverActive = false;
    bool hoverTriggerLane = false;
    int hoverStripIndex = -1;
    int hoverLaneIndex = -1;
    int hoverEventIndex = -1;
    bool hoverTriggerMoveTime = true;
    bool hoverTriggerMoveOffset = true;
    double hoverBeat = 0.0;
    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
        0.0f,
        0.0f,
        static_cast<float>(sceneTimelineCanvas.getWidth()),
        sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
        sceneGlobalLaneExpanded);
    const int globalLane = sceneGlobalAutomationLaneIndex();
    if (globalLane >= 0 && globalLayout.laneBounds.contains(position))
    {
        hoverActive = true;
        hoverTriggerLane = false;
        hoverStripIndex = -1;
        hoverLaneIndex = globalLane;
        hoverBeat = sceneTimeBeatsForX(globalLayout.laneBounds, position.x, lengthBeats);

        for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            if (sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats).expanded(3.0f, 3.0f).contains(position))
            {
                hoverEventIndex = eventIndex;
                break;
            }
        }
    }

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount() && !hoverActive; ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }
        if (layout.compactOverview)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        const auto triggerInteractionBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
            ? layout.stepLaunchBounds
            : layout.triggerTimelineBounds;

        if (triggerInteractionBounds.contains(position))
        {
            hoverActive = true;
            hoverTriggerLane = true;
            hoverStripIndex = visibleStrip;
            hoverBeat = sceneTimeBeatsForX(triggerInteractionBounds, position.x, lengthBeats);

            for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
            {
                const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                if (event.type != ScenePerformanceEventType::Trigger || event.stripIndex != visibleStrip)
                    continue;

                bool moveTime = true;
                bool moveOffset = true;
                if (!sceneResolveTriggerDragIntent(layout, event, lengthBeats, position, moveTime, moveOffset))
                    continue;

                hoverEventIndex = eventIndex;
                hoverTriggerMoveTime = moveTime;
                hoverTriggerMoveOffset = moveOffset;
                break;
            }
        }
        else if (layout.automationExpanded)
        {
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                if (!laneBounds.contains(position))
                    continue;

                hoverActive = true;
                hoverTriggerLane = false;
                hoverStripIndex = visibleStrip;
                hoverLaneIndex = lane;
                hoverBeat = sceneTimeBeatsForX(laneBounds, position.x, lengthBeats);

                for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
                {
                    const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                    if (event.type != ScenePerformanceEventType::ControlPoint
                        || event.stripIndex != visibleStrip
                        || sceneAutomationLaneIndex(event) != lane)
                    {
                        continue;
                    }

                    if (sceneControlMarkerBounds(layout, event, lengthBeats).expanded(3.0f, 3.0f).contains(position))
                    {
                        hoverEventIndex = eventIndex;
                        break;
                    }
                }
                break;
            }
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    if (hoverActive && snapToGrid)
        hoverBeat = snapSceneBeatToGrid(hoverBeat, lengthBeats);

    const bool changed = sceneEditorState.hoverActive != hoverActive
        || sceneEditorState.hoverTriggerLane != hoverTriggerLane
        || sceneEditorState.hoverStripIndex != hoverStripIndex
        || sceneEditorState.hoverLaneIndex != hoverLaneIndex
        || sceneEditorState.hoverEventIndex != hoverEventIndex
        || sceneEditorState.hoverTriggerMoveTime != hoverTriggerMoveTime
        || sceneEditorState.hoverTriggerMoveOffset != hoverTriggerMoveOffset
        || std::abs(sceneEditorState.hoverBeat - hoverBeat) > 1.0e-6;

    if (changed)
    {
        sceneEditorState.hoverActive = hoverActive;
        sceneEditorState.hoverTriggerLane = hoverTriggerLane;
        sceneEditorState.hoverStripIndex = hoverStripIndex;
        sceneEditorState.hoverLaneIndex = hoverLaneIndex;
        sceneEditorState.hoverEventIndex = hoverEventIndex;
        sceneEditorState.hoverTriggerMoveTime = hoverTriggerMoveTime;
        sceneEditorState.hoverTriggerMoveOffset = hoverTriggerMoveOffset;
        sceneEditorState.hoverBeat = hoverBeat;
        sceneTimelineCanvas.repaint();
    }

    juce::MouseCursor cursor(juce::MouseCursor::NormalCursor);
    if (!processor.isScenePerformanceRecording())
    {
        if (sceneDrawModeEnabled && hoverActive)
        {
            cursor = juce::MouseCursor::CrosshairCursor;
        }
        else if (hoverEventIndex >= 0)
        {
            if (hoverTriggerLane)
            {
                if (hoverTriggerMoveTime && hoverTriggerMoveOffset)
                    cursor = juce::MouseCursor::DraggingHandCursor;
                else if (hoverTriggerMoveTime)
                    cursor = juce::MouseCursor::LeftRightResizeCursor;
                else if (hoverTriggerMoveOffset)
                    cursor = juce::MouseCursor::UpDownResizeCursor;
            }
            else
            {
                cursor = juce::MouseCursor::DraggingHandCursor;
            }
        }
    }

    sceneTimelineCanvas.setMouseCursor(cursor);
}

void SceneControlPanel::clearSceneHoverState()
{
    const bool wasActive = sceneEditorState.hoverActive
        || sceneEditorState.hoverEventIndex >= 0;

    sceneEditorState.hoverActive = false;
    sceneEditorState.hoverTriggerLane = false;
    sceneEditorState.hoverStripIndex = -1;
    sceneEditorState.hoverLaneIndex = -1;
    sceneEditorState.hoverEventIndex = -1;
    sceneEditorState.hoverTriggerMoveTime = true;
    sceneEditorState.hoverTriggerMoveOffset = true;
    sceneEditorState.hoverBeat = 0.0;
    sceneTimelineCanvas.setMouseCursor(juce::MouseCursor::NormalCursor);
    if (wasActive)
        sceneTimelineCanvas.repaint();
}

void SceneControlPanel::updateSceneViewportFollow()
{
    if (!sceneFollowPlayheadEnabled
        || !isShowing()
        || sceneEditorState.transportProgress < 0.0f
        || sceneViewport.getWidth() <= 0
        || sceneTimelineCanvas.getWidth() <= sceneViewport.getWidth())
    {
        return;
    }

    const auto canvasBounds = sceneTimelineCanvas.getLocalBounds().toFloat();
    const auto layout = makeSceneStripCardLayout(
        processor,
        0,
        juce::Rectangle<float>(0.0f,
                               0.0f,
                               canvasBounds.getWidth(),
                               sceneStripCardHeight(processor,
                                                    0,
                                                    stripAutomationExpanded.front(),
                                                    stripHeightExpanded.front())),
        stripAutomationExpanded.front(),
        stripHeightExpanded.front());
    const float headX = layout.triggerTimelineBounds.getX()
        + (layout.triggerTimelineBounds.getWidth() * sceneEditorState.transportProgress);
    const int viewportWidth = sceneViewport.getWidth();
    const int currentX = sceneViewport.getViewPositionX();
    const int margin = juce::jmax(48, viewportWidth / 5);
    const int minVisibleX = currentX + margin;
    const int maxVisibleX = currentX + viewportWidth - margin;
    const int headXi = static_cast<int>(std::round(headX));

    if (headXi >= minVisibleX && headXi <= maxVisibleX)
        return;

    const int maxX = juce::jmax(0, sceneTimelineCanvas.getWidth() - viewportWidth);
    const int targetX = juce::jlimit(0, maxX, headXi - (viewportWidth / 2));
    sceneViewport.setViewPosition(targetX, sceneViewport.getViewPositionY());
}

void SceneControlPanel::updateSceneTimelineContentSize()
{
    const int baseWidth = juce::jmax(220, sceneViewport.getWidth() - sceneViewport.getScrollBarThickness());
    const int zoomedWidth = juce::jmax(baseWidth, baseWidth * juce::jmax(1, sceneZoomFactor));
    const int contentHeight = getSceneTimelineContentHeight();
    const int currentX = sceneViewport.getViewPositionX();
    sceneTimelineCanvas.setSize(zoomedWidth, contentHeight);

    const int maxX = juce::jmax(0, sceneTimelineCanvas.getWidth() - sceneViewport.getWidth());
    if (currentX > maxX)
        sceneViewport.setViewPosition(maxX, sceneViewport.getViewPositionY());

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int stripIndex = 0; stripIndex < SceneEditorVisibleStrips; ++stripIndex)
    {
        const bool stripVisible = stripIndex < getVisibleSceneStripCount();
        for (int lane = 0; lane < SceneAutomationLaneCount; ++lane)
            sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)].setVisible(false);
        if (!stripVisible)
            continue;

        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 stripIndex,
                                 stripAutomationExpanded[static_cast<size_t>(stripIndex)],
                                 stripHeightExpanded[static_cast<size_t>(stripIndex)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     stripIndex,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(stripIndex)],
                                                     stripHeightExpanded[static_cast<size_t>(stripIndex)]);
        const bool showMotionTargets = shouldShowSceneMotionTargetSelectors(stripIndex);
        for (int lane = 0; lane < SceneAutomationLaneCount; ++lane)
        {
            auto& targetBox = sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)];
            const auto bounds = layout.motionTargetBounds[static_cast<size_t>(lane)].toNearestInt();
            targetBox.setBounds(bounds);
            targetBox.setVisible(showMotionTargets && !bounds.isEmpty());
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    if (sceneFollowPlayheadEnabled)
        updateSceneViewportFollow();
}

double SceneControlPanel::getSceneTimelineLengthBeats(int sceneSlot) const
{
    const double clipLengthBeats = processor.getScenePerformanceClipLengthBeats(sceneSlot);
    const double resolvedLengthBeats = processor.getResolvedSceneLengthBeats(sceneSlot);
    return juce::jmax(1.0, juce::jmax(clipLengthBeats, resolvedLengthBeats));
}

double SceneControlPanel::snapSceneBeatToGrid(double beat, double lengthBeats) const
{
    const double maxBeat = juce::jmax(0.0, std::nextafter(lengthBeats, 0.0));
    double clampedBeat = juce::jlimit(0.0, maxBeat, beat);
    const bool useGrid = sceneGridEnabled || sceneDrawModeEnabled;
    if (!useGrid)
        return clampedBeat;

    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const double gridStepBeats = 4.0 / static_cast<double>(safeDivision);
    if (!(gridStepBeats > 0.0) || !std::isfinite(gridStepBeats))
        return clampedBeat;

    clampedBeat = std::round(clampedBeat / gridStepBeats) * gridStepBeats;
    return juce::jlimit(0.0, maxBeat, clampedBeat);
}

bool SceneControlPanel::applySceneDrawTrigger(int stripIndex, double timeBeats, int column)
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    ScenePerformanceEvent drawnEvent;
    drawnEvent.type = ScenePerformanceEventType::Trigger;
    drawnEvent.stripIndex = safeStripIndex;
    drawnEvent.timeBeats = snapSceneBeatToGrid(timeBeats, lengthBeats);
    const int resolvedColumn = column >= 0 ? column : defaultStepTriggerColumnForStrip(safeStripIndex);
    drawnEvent.column = juce::jlimit(0, 15, resolvedColumn);
    drawnEvent.isNoteOn = true;
    drawnEvent.sampleSliceId = -1;
    drawnEvent.sampleStartSample = -1;

    auto events = sceneEditorState.events;
    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const bool useGrid = sceneGridEnabled || sceneDrawModeEnabled;
    const double gridStepBeats = useGrid ? (4.0 / static_cast<double>(safeDivision)) : 0.0;
    const double matchEpsilon = useGrid ? juce::jmax(1.0e-4, gridStepBeats * 0.45) : 1.0e-3;

    for (auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::Trigger || event.stripIndex != drawnEvent.stripIndex)
            continue;

        if (std::abs(event.timeBeats - drawnEvent.timeBeats) <= matchEpsilon)
        {
            event = drawnEvent;
            return applyEditedSceneEvents(std::move(events), -1, &drawnEvent);
        }
    }

    events.push_back(drawnEvent);
    return applyEditedSceneEvents(std::move(events), -1, &drawnEvent);
}

bool SceneControlPanel::applySceneDrawPoint(int stripIndex, int laneIndex, double timeBeats, float normalizedValue)
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const int safeLaneIndex = juce::jlimit(0, kSceneAutomationLaneCount - 1, laneIndex);
    const int safeStripIndex = sceneResolveAutomationStripIndex(stripIndex, safeLaneIndex);
    const bool drawStepped = sceneDrawModeEnabled;
    auto drawnEvent = makeDefaultSceneControlEventForLane(safeStripIndex,
                                                          safeLaneIndex,
                                                          snapSceneBeatToGrid(timeBeats, lengthBeats),
                                                          normalizedValue,
                                                          drawStepped);

    auto events = sceneEditorState.events;
    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const bool useGrid = sceneGridEnabled || sceneDrawModeEnabled;
    const double gridStepBeats = useGrid ? (4.0 / static_cast<double>(safeDivision)) : 0.0;
    const double matchEpsilon = useGrid ? juce::jmax(1.0e-4, gridStepBeats * 0.45) : 1.0e-3;

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeLaneIndex, safeStripIndex, &drawnEvent, matchEpsilon](const ScenePerformanceEvent& event)
                                {
                                    if (event.type != ScenePerformanceEventType::ControlPoint
                                        || sceneAutomationLaneIndex(event) != safeLaneIndex)
                                    {
                                        return false;
                                    }

                                    if (!sceneAutomationLaneUsesGlobalStrip(safeLaneIndex)
                                        && event.stripIndex != safeStripIndex)
                                    {
                                        return false;
                                    }

                                    return std::abs(event.timeBeats - drawnEvent.timeBeats) <= matchEpsilon;
                                }),
                 events.end());

    events.push_back(drawnEvent);
    return applyEditedSceneEvents(std::move(events), -1, &drawnEvent);
}

bool SceneControlPanel::applySceneDrawCurveSegment(int stripIndex,
                                                   int laneIndex,
                                                   double startBeat,
                                                   float startValue,
                                                   double endBeat,
                                                   float endValue)
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const int safeLaneIndex = juce::jlimit(0, kSceneAutomationLaneCount - 1, laneIndex);
    const int safeStripIndex = sceneResolveAutomationStripIndex(stripIndex, safeLaneIndex);
    const bool steppedHoldDrawing = sceneDrawModeEnabled && sceneEditorState.drawActive;
    const double maxBeat = juce::jmax(0.0, std::nextafter(juce::jmax(1.0, lengthBeats), 0.0));
    const double clampedStartBeat = juce::jlimit(0.0, maxBeat, startBeat);
    const double clampedEndBeat = juce::jlimit(0.0, maxBeat, endBeat);

    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const double gridStepBeats = (sceneGridEnabled || steppedHoldDrawing)
        ? (4.0 / static_cast<double>(safeDivision))
        : 0.0;
    if (!(gridStepBeats > 0.0) || !std::isfinite(gridStepBeats))
    {
        if (steppedHoldDrawing)
            return applySceneDrawPoint(safeStripIndex, safeLaneIndex, clampedEndBeat, endValue);

        if (std::abs(clampedStartBeat - clampedEndBeat) <= 1.0e-3)
            return applySceneDrawPoint(safeStripIndex, safeLaneIndex, clampedEndBeat, endValue);

        auto events = sceneEditorState.events;
        const double segmentStart = juce::jmin(clampedStartBeat, clampedEndBeat);
        const double segmentEnd = juce::jmax(clampedStartBeat, clampedEndBeat);
        const double matchEpsilon = 1.0e-3;
        auto drawnEvent = makeDefaultSceneControlEventForLane(safeStripIndex,
                                                              safeLaneIndex,
                                                              clampedEndBeat,
                                                              endValue,
                                                              false);

        events.erase(std::remove_if(events.begin(),
                                    events.end(),
                                    [safeStripIndex,
                                     safeLaneIndex,
                                     segmentStart,
                                     segmentEnd,
                                     clampedStartBeat,
                                     clampedEndBeat,
                                     matchEpsilon](const ScenePerformanceEvent& event)
                                    {
                                        if (event.type != ScenePerformanceEventType::ControlPoint
                                            || sceneAutomationLaneIndex(event) != safeLaneIndex)
                                        {
                                            return false;
                                        }

                                        if (!sceneAutomationLaneUsesGlobalStrip(safeLaneIndex)
                                            && event.stripIndex != safeStripIndex)
                                        {
                                            return false;
                                        }

                                        if (std::abs(event.timeBeats - clampedStartBeat) <= matchEpsilon)
                                            return false;

                                        if (std::abs(event.timeBeats - clampedEndBeat) <= matchEpsilon)
                                            return true;

                                        return event.timeBeats > (segmentStart + matchEpsilon)
                                            && event.timeBeats < (segmentEnd - matchEpsilon);
                                    }),
                     events.end());

        events.push_back(drawnEvent);
        return applyEditedSceneEvents(std::move(events), -1, &drawnEvent);
    }

    const auto snapDrawBeat = [gridStepBeats, maxBeat](double beat)
    {
        const double clampedBeat = juce::jlimit(0.0, maxBeat, beat);
        const double steppedBeat = std::round(clampedBeat / gridStepBeats) * gridStepBeats;
        return juce::jlimit(0.0, maxBeat, steppedBeat);
    };

    const double snappedStart = snapDrawBeat(startBeat);
    const double snappedEnd = snapDrawBeat(endBeat);
    const double segmentStart = juce::jmin(snappedStart, snappedEnd);
    const double segmentEnd = juce::jmax(snappedStart, snappedEnd);
    const double matchEpsilon = juce::jmax(1.0e-4, gridStepBeats * 0.45);

    auto events = sceneEditorState.events;
    std::vector<ScenePerformanceEvent> drawnEvents;
    if (steppedHoldDrawing)
    {
        std::vector<double> beatsToWrite;
        if (std::abs(snappedEnd - snappedStart) <= matchEpsilon)
        {
            beatsToWrite.push_back(snappedEnd);
        }
        else
        {
            const double direction = snappedEnd > snappedStart ? 1.0 : -1.0;
            for (double beat = snappedStart + (direction * gridStepBeats);
                 direction > 0.0 ? beat <= (snappedEnd + matchEpsilon) : beat >= (snappedEnd - matchEpsilon);
                 beat += direction * gridStepBeats)
            {
                const double clampedBeat = snapDrawBeat(beat);
                if (beatsToWrite.empty() || std::abs(beatsToWrite.back() - clampedBeat) > matchEpsilon)
                    beatsToWrite.push_back(clampedBeat);
            }

            if (beatsToWrite.empty())
                beatsToWrite.push_back(snappedEnd);
        }

        events.erase(std::remove_if(events.begin(),
                                    events.end(),
                                    [safeStripIndex, safeLaneIndex, &beatsToWrite, matchEpsilon](const ScenePerformanceEvent& event)
                                    {
                                        if (event.type != ScenePerformanceEventType::ControlPoint
                                            || sceneAutomationLaneIndex(event) != safeLaneIndex)
                                        {
                                            return false;
                                        }

                                        if (!sceneAutomationLaneUsesGlobalStrip(safeLaneIndex)
                                            && event.stripIndex != safeStripIndex)
                                        {
                                            return false;
                                        }

                                        return std::any_of(beatsToWrite.begin(),
                                                           beatsToWrite.end(),
                                                           [&event, matchEpsilon](double beat)
                                                           {
                                                               return std::abs(event.timeBeats - beat) <= matchEpsilon;
                                                           });
                                    }),
                     events.end());

        const float normalized = juce::jlimit(0.0f, 1.0f, endValue);
        for (double beat : beatsToWrite)
        {
            auto drawnEvent = makeDefaultSceneControlEventForLane(safeStripIndex,
                                                                  safeLaneIndex,
                                                                  beat,
                                                                  normalized,
                                                                  true);
            events.push_back(drawnEvent);
            drawnEvents.push_back(drawnEvent);
        }
    }
    else
    {
        events.erase(std::remove_if(events.begin(),
                                    events.end(),
                                    [safeStripIndex, safeLaneIndex, segmentStart, segmentEnd, matchEpsilon](const ScenePerformanceEvent& event)
                                    {
                                        return event.type == ScenePerformanceEventType::ControlPoint
                                            && sceneAutomationLaneIndex(event) == safeLaneIndex
                                            && (!sceneAutomationLaneUsesGlobalStrip(safeLaneIndex)
                                                    ? (event.stripIndex == safeStripIndex)
                                                    : true)
                                            && event.timeBeats >= (segmentStart - matchEpsilon)
                                            && event.timeBeats <= (segmentEnd + matchEpsilon);
                                    }),
                     events.end());

        const double span = std::abs(snappedEnd - snappedStart);
        for (double beat = segmentStart; beat <= (segmentEnd + matchEpsilon); beat += gridStepBeats)
        {
            const double clampedBeat = snapDrawBeat(beat);
            const double t = span <= 1.0e-6
                ? 1.0
                : juce::jlimit(0.0, 1.0, (clampedBeat - snappedStart) / (snappedEnd - snappedStart));
            const float normalized = juce::jlimit(0.0f,
                                                  1.0f,
                                                  startValue + ((endValue - startValue) * static_cast<float>(t)));
            auto drawnEvent = makeDefaultSceneControlEventForLane(safeStripIndex,
                                                                  safeLaneIndex,
                                                                  clampedBeat,
                                                                  normalized,
                                                                  false);
            events.push_back(drawnEvent);
            drawnEvents.push_back(drawnEvent);
        }
    }

    if (drawnEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events), -1, &drawnEvents.back(), &drawnEvents);
}

bool SceneControlPanel::eraseSceneLaneEventAt(int stripIndex, int laneIndex, bool triggerLane, double timeBeats)
{
    if (processor.isScenePerformanceRecording())
        return false;

    auto events = sceneEditorState.events;
    if (events.empty())
        return false;

    const int safeStripIndex = triggerLane
        ? juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex)
        : sceneResolveAutomationStripIndex(stripIndex, laneIndex);
    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const double gridStepBeats = sceneGridEnabled ? (4.0 / static_cast<double>(safeDivision)) : 0.0;
    const double snappedBeat = snapSceneBeatToGrid(timeBeats, getSceneTimelineLengthBeats(getFocusedSceneSlot()));
    const double matchEpsilon = sceneGridEnabled ? juce::jmax(1.0e-4, gridStepBeats * 0.45) : 1.0e-3;

    auto removeIt = std::find_if(events.begin(),
                                 events.end(),
                                 [safeStripIndex, laneIndex, triggerLane, snappedBeat, matchEpsilon](const ScenePerformanceEvent& event)
                                 {
                                     if (triggerLane)
                                         return event.stripIndex == safeStripIndex
                                             && event.type == ScenePerformanceEventType::Trigger
                                             && std::abs(event.timeBeats - snappedBeat) <= matchEpsilon;
                                     return event.type == ScenePerformanceEventType::ControlPoint
                                         && (sceneAutomationLaneUsesGlobalStrip(laneIndex)
                                                ? sceneIsGlobalAutomationEvent(event)
                                                : (event.stripIndex == safeStripIndex))
                                         && sceneAutomationLaneIndex(event) == laneIndex
                                         && std::abs(event.timeBeats - snappedBeat) <= matchEpsilon;
                                 });

    if (removeIt == events.end())
        return false;

    events.erase(removeIt);
    return applyEditedSceneEvents(std::move(events));
}

bool SceneControlPanel::clearSceneLane(int stripIndex, int laneIndex, bool triggerLane)
{
    auto events = sceneEditorState.events;
    const int safeStripIndex = triggerLane
        ? juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex)
        : sceneResolveAutomationStripIndex(stripIndex, laneIndex);
    const auto oldSize = events.size();

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeStripIndex, laneIndex, triggerLane](const ScenePerformanceEvent& event)
                                {
                                    if (triggerLane)
                                        return event.stripIndex == safeStripIndex
                                            && event.type == ScenePerformanceEventType::Trigger;
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && (sceneAutomationLaneUsesGlobalStrip(laneIndex)
                                               ? sceneIsGlobalAutomationEvent(event)
                                               : (event.stripIndex == safeStripIndex))
                                        && sceneAutomationLaneIndex(event) == laneIndex;
                                }),
                 events.end());

    if (events.size() == oldSize)
        return false;

    return applyEditedSceneEvents(std::move(events));
}

bool SceneControlPanel::defaultSceneLane(int stripIndex, int laneIndex)
{
    if (processor.isScenePerformanceRecording())
        return false;

    auto events = sceneEditorState.events;
    const int safeLaneIndex = juce::jlimit(0, kSceneAutomationLaneCount - 1, laneIndex);
    const int safeStripIndex = sceneResolveAutomationStripIndex(stripIndex, safeLaneIndex);
    const auto oldSize = events.size();

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeStripIndex, safeLaneIndex](const ScenePerformanceEvent& event)
                                {
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && sceneAutomationLaneIndex(event) == safeLaneIndex
                                        && (sceneAutomationLaneUsesGlobalStrip(safeLaneIndex)
                                               ? sceneIsGlobalAutomationEvent(event)
                                               : (event.stripIndex == safeStripIndex));
                                }),
                 events.end());

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const double endBeat = sceneAutomationWriteEndBeat(lengthBeats);
    float normalizedValue = sceneAutomationLaneIsBipolar(safeLaneIndex)
        ? 0.5f
        : sceneDefaultNormalizedValueForLane(safeLaneIndex);
    const auto target = sceneAutomationLaneTarget(safeLaneIndex);
    if (sceneCurrentBaseNormalizedValue(processor,
                                        sceneAutomationLaneUsesGlobalStrip(safeLaneIndex) ? -1 : safeStripIndex,
                                        target,
                                        normalizedValue))
    {
        normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);
    }

    std::vector<ScenePerformanceEvent> insertedEvents;
    insertedEvents.push_back(makeDefaultSceneControlEventForLane(safeStripIndex,
                                                                 safeLaneIndex,
                                                                 0.0,
                                                                 normalizedValue));
    if (endBeat > 0.0)
    {
        insertedEvents.push_back(makeDefaultSceneControlEventForLane(safeStripIndex,
                                                                     safeLaneIndex,
                                                                     endBeat,
                                                                     normalizedValue));
    }

    events.insert(events.end(), insertedEvents.begin(), insertedEvents.end());

    if (events.size() == oldSize && insertedEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events), -1, &insertedEvents.front(), &insertedEvents);
}

bool SceneControlPanel::thinSceneLane(int stripIndex, int laneIndex, bool triggerLane)
{
    auto events = sceneEditorState.events;
    if (events.empty())
        return false;

    const int safeStripIndex = triggerLane
        ? juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex)
        : sceneResolveAutomationStripIndex(stripIndex, laneIndex);
    const int safeDivision = juce::jlimit(1, 64, sceneGridDivision);
    const double gridStepBeats = sceneGridEnabled ? (4.0 / static_cast<double>(safeDivision)) : 0.25;
    const double timeThreshold = juce::jmax(1.0e-4, gridStepBeats * 0.5);
    const float valueThreshold = 0.015f;

    std::vector<ScenePerformanceEvent> targetEvents;
    std::vector<ScenePerformanceEvent> passthroughEvents;
    targetEvents.reserve(events.size());
    passthroughEvents.reserve(events.size());

    for (const auto& event : events)
    {
        const bool matchesLane = triggerLane
            ? (event.stripIndex == safeStripIndex && event.type == ScenePerformanceEventType::Trigger)
            : (event.type == ScenePerformanceEventType::ControlPoint
               && (sceneAutomationLaneUsesGlobalStrip(laneIndex)
                       ? sceneIsGlobalAutomationEvent(event)
                       : (event.stripIndex == safeStripIndex))
               && sceneAutomationLaneIndex(event) == laneIndex);
        if (matchesLane)
            targetEvents.push_back(event);
        else
            passthroughEvents.push_back(event);
    }

    if (targetEvents.size() <= 1)
        return false;

    std::sort(targetEvents.begin(), targetEvents.end());
    std::vector<ScenePerformanceEvent> thinned;
    thinned.reserve(targetEvents.size());

    if (triggerLane)
    {
        double lastKeptBeat = std::numeric_limits<double>::quiet_NaN();
        for (const auto& event : targetEvents)
        {
            if (!std::isfinite(lastKeptBeat) || std::abs(event.timeBeats - lastKeptBeat) > timeThreshold)
            {
                thinned.push_back(event);
                lastKeptBeat = event.timeBeats;
            }
        }
    }
    else
    {
        thinned.push_back(targetEvents.front());
        for (size_t i = 1; i + 1 < targetEvents.size(); ++i)
        {
            const auto& previousKept = thinned.back();
            const auto& current = targetEvents[i];
            const auto& next = targetEvents[i + 1];
            const bool nearPreviousInTime = std::abs(current.timeBeats - previousKept.timeBeats) <= timeThreshold;
            const bool nearPreviousInValue = std::abs(normalizeSceneAutomationValue(current)
                                                      - normalizeSceneAutomationValue(previousKept)) <= valueThreshold;
            const bool nearNextInValue = std::abs(normalizeSceneAutomationValue(current)
                                                  - normalizeSceneAutomationValue(next)) <= valueThreshold;
            if (nearPreviousInTime && nearPreviousInValue && nearNextInValue)
                continue;
            thinned.push_back(current);
        }
        thinned.push_back(targetEvents.back());
    }

    if (thinned.size() == targetEvents.size())
        return false;

    passthroughEvents.insert(passthroughEvents.end(), thinned.begin(), thinned.end());
    return applyEditedSceneEvents(std::move(passthroughEvents));
}

void SceneControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    const auto comboFont = juce::Font(juce::FontOptions(12.0f, juce::Font::bold));
    auto comboWidthForTexts = [&comboFont](std::initializer_list<juce::String> labels, int minWidth, int maxWidth)
    {
        int textWidth = 0;
        for (const auto& label : labels)
            textWidth = juce::jmax(textWidth, juce::GlyphArrangement::getStringWidthInt(comboFont, label));
        return juce::jlimit(minWidth, maxWidth, textWidth + 34);
    };

    auto titleRow = bounds.removeFromTop(18);
    sceneModeToggle.setBounds(titleRow.removeFromRight(106));
    titleLabel.setBounds(titleRow);

    hintLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(5);

    auto compactButtonWidthForText = [&comboFont](const juce::String& label, int minWidth, int maxWidth)
    {
        const int textWidth = juce::GlyphArrangement::getStringWidthInt(comboFont, label);
        return juce::jlimit(minWidth, maxWidth, textWidth + 20);
    };

    const int settingsHeight = juce::jlimit(198, 214, static_cast<int>(std::round(getHeight() * 0.296f)));
    auto settingsBounds = bounds.removeFromTop(settingsHeight);
    const int sectionGap = 6;
    const int sceneButtonGap = 10;
    const int controlHeight = 22;

    sceneSlotsSectionLabel.setBounds({});
    settingsBounds.removeFromTop(2);

    auto sceneRow = settingsBounds.removeFromTop(26);
    const int rowGap = 4;
    const int rowCellCount = MlrVSTAudioProcessor::SceneSlots;
    const int rowCellWidth = juce::jmax(38, (sceneRow.getWidth() - (rowGap * (rowCellCount - 1))) / rowCellCount);
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        sceneSelectorButtons[static_cast<size_t>(sceneSlot)].setBounds(sceneRow.removeFromLeft(rowCellWidth));
        if (sceneSlot < rowCellCount - 1)
            sceneRow.removeFromLeft(rowGap);
    }
    const auto sceneContentX = sceneSelectorButtons.front().getX();
    const auto sceneContentWidth = sceneSelectorButtons.back().getRight() - sceneContentX;
    sceneChainPlayButton.setBounds({});
    sceneRecordButton.setBounds({});
    sceneSceneCaptureButton.setBounds({});

    settingsBounds.removeFromTop(sceneButtonGap);

    auto sceneActionRow = settingsBounds.removeFromTop(controlHeight);
    sceneActionRow.setX(sceneContentX);
    sceneActionRow.setWidth(sceneContentWidth);
    const int actionGap = 4;
    const int beforeWidth = compactButtonWidthForText("Before", 50, 56);
    const int afterWidth = compactButtonWidthForText("After", 46, 52);
    const int copyWidth = compactButtonWidthForText("Copy", 42, 48);
    const int pasteWidth = compactButtonWidthForText("Paste", 44, 50);
    const int doubleWidth = compactButtonWidthForText("Double", 48, 56);
    const int resetWidth = compactButtonWidthForText("Reset", 44, 50);
    const int utilityRowWidth = beforeWidth + afterWidth + copyWidth + pasteWidth + doubleWidth + resetWidth
        + (actionGap * 5);
    sceneSlotHeaderLabel.setBounds({});
    scenePlaybackSectionLabel.setBounds({});

    auto utilityRow = sceneActionRow.removeFromRight(utilityRowWidth);
    sceneActionRow.removeFromRight(10);

    sceneChainClearButton.setBounds(utilityRow.removeFromRight(resetWidth));
    utilityRow.removeFromRight(actionGap);
    sceneDuplicateLengthButton.setBounds(utilityRow.removeFromRight(doubleWidth));
    utilityRow.removeFromRight(actionGap);
    sceneScenePasteButton.setBounds(utilityRow.removeFromRight(pasteWidth));
    utilityRow.removeFromRight(actionGap);
    sceneSceneCopyButton.setBounds(utilityRow.removeFromRight(copyWidth));
    utilityRow.removeFromRight(actionGap);
    sceneInsertAfterButton.setBounds(utilityRow.removeFromRight(afterWidth));
    utilityRow.removeFromRight(actionGap);
    sceneInsertBeforeButton.setBounds(utilityRow.removeFromRight(beforeWidth));

    auto timingRow = sceneActionRow;
    const int labelGap = 5;
    const int transportWidth = compactButtonWidthForText("STOP", 64, 74);
    const int advanceLabelWidth = 34;
    const int advanceWidth = comboWidthForTexts({"Grid", "Pattern", "Scene", "Manual"},
                                                66,
                                                juce::jmax(70, timingRow.getWidth() / 8));
    const int lengthLabelWidth = 44;
    const int lengthWidth = comboWidthForTexts({"Longest Strip", "Pattern End", "Bars", "Anchor Strip"},
                                               80,
                                               juce::jmax(84, timingRow.getWidth() / 6));
    const int countLabelWidth = 52;
    const int countWidth = comboWidthForTexts({juce::String(MlrVSTAudioProcessor::MaxSceneManualBars)},
                                              44,
                                              juce::jmax(52, timingRow.getWidth() / 9));
    const int anchorLabelWidth = 48;
    const int anchorWidth = comboWidthForTexts({juce::String("S") + juce::String(MlrVSTAudioProcessor::MaxStrips)},
                                               42,
                                               juce::jmax(44, timingRow.getWidth() / 10));
    sceneChainPlayButton.setBounds(timingRow.removeFromLeft(transportWidth));
    timingRow.removeFromLeft(labelGap + 3);
    sceneChangeModeLabel.setBounds(timingRow.removeFromLeft(advanceLabelWidth));
    timingRow.removeFromLeft(labelGap);
    sceneChangeModeBox.setBounds(timingRow.removeFromLeft(advanceWidth));
    timingRow.removeFromLeft(labelGap + 1);
    sceneLengthHeaderLabel.setBounds(timingRow.removeFromLeft(lengthLabelWidth));
    timingRow.removeFromLeft(labelGap);
    const int focusedSceneSlot = getFocusedSceneSlot();
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        sceneLengthModeBoxes[static_cast<size_t>(sceneSlot)].setBounds({});
        sceneManualBarsBoxes[static_cast<size_t>(sceneSlot)].setBounds({});
        sceneAnchorStripBoxes[static_cast<size_t>(sceneSlot)].setBounds({});
    }
    sceneLengthModeBoxes[static_cast<size_t>(focusedSceneSlot)].setBounds(timingRow.removeFromLeft(lengthWidth));
    timingRow.removeFromLeft(labelGap);
    sceneBarsHeaderLabel.setBounds(timingRow.removeFromLeft(countLabelWidth));
    timingRow.removeFromLeft(labelGap);
    sceneManualBarsBoxes[static_cast<size_t>(focusedSceneSlot)].setBounds(timingRow.removeFromLeft(countWidth));
    timingRow.removeFromLeft(labelGap);
    sceneAnchorHeaderLabel.setBounds(timingRow.removeFromLeft(anchorLabelWidth));
    timingRow.removeFromLeft(labelGap);
    sceneAnchorStripBoxes[static_cast<size_t>(focusedSceneSlot)].setBounds(timingRow.removeFromLeft(anchorWidth));
    sceneTimingLayoutFocusedSlot = focusedSceneSlot;

    settingsBounds.removeFromTop(sectionGap);
    sceneFillSectionLabel.setBounds({});

    auto chainRow = settingsBounds.removeFromTop(60);
    chainRow.setX(sceneContentX);
    chainRow.setWidth(sceneContentWidth);
    sceneChainCanvas.setBounds(chainRow);

    settingsBounds.removeFromTop(sectionGap);

    auto transitionRow = settingsBounds.removeFromTop(controlHeight);
    transitionRow.setX(sceneContentX);
    transitionRow.setWidth(sceneContentWidth);
    const int fillCellGap = 6;
    const int fillCellWidth = (transitionRow.getWidth() - (fillCellGap * 3)) / 4;
    auto takeFillCell = [&](juce::Rectangle<int>& row, bool lastCell = false)
    {
        auto cell = lastCell ? row : row.removeFromLeft(fillCellWidth);
        if (!lastCell && row.getWidth() > 0)
            row.removeFromLeft(fillCellGap);
        return cell;
    };
    auto layoutFillField = [&](juce::Rectangle<int> cell, juce::Label& label, juce::Component& component, int labelWidth)
    {
        label.setBounds(cell.removeFromLeft(labelWidth));
        cell.removeFromLeft(4);
        component.setBounds(cell);
    };
    layoutFillField(takeFillCell(transitionRow), sceneTransitionHeaderLabel, sceneTransitionTypeBox, 34);
    layoutFillField(takeFillCell(transitionRow), sceneTransitionOptionsHeaderLabel, sceneTransitionOptionsBox, 34);
    layoutFillField(takeFillCell(transitionRow), sceneTransitionLengthHeaderLabel, sceneTransitionLengthBox, 34);
    sceneTransitionSubtractButton.setBounds(takeFillCell(transitionRow, true));
    sceneTimingLayoutFocusedStep = getFocusedSceneChainStep();

    settingsBounds.removeFromTop(4);

    auto transitionParamRow = settingsBounds.removeFromTop(controlHeight);
    transitionParamRow.setX(sceneContentX);
    transitionParamRow.setWidth(sceneContentWidth);
    const int transitionParamLabelWidth = 36;
    auto layoutTransitionParam = [&](juce::Rectangle<int> cell, juce::Label& label, juce::Component& component)
    {
        label.setBounds(cell.removeFromLeft(transitionParamLabelWidth));
        cell.removeFromLeft(4);
        component.setBounds(cell);
    };
    layoutTransitionParam(takeFillCell(transitionParamRow), sceneTransitionMixHeaderLabel, sceneTransitionMixSlider);
    layoutTransitionParam(takeFillCell(transitionParamRow), sceneTransitionDelayHeaderLabel, sceneTransitionDelaySlider);
    layoutTransitionParam(takeFillCell(transitionParamRow), sceneTransitionFilterHeaderLabel, sceneTransitionFilterSlider);
    layoutTransitionParam(takeFillCell(transitionParamRow, true), sceneTransitionChopHeaderLabel, sceneTransitionChopSlider);

    settingsBounds.removeFromTop(4);

    sceneTransitionSmartLinkButton.setBounds({});
    sceneTransitionPunchButton.setBounds({});
    sceneTransitionLiftButton.setBounds({});
    sceneTransitionEchoButton.setBounds({});
    sceneTransitionDropButton.setBounds({});
    sceneTransitionScopeHeaderLabel.setBounds({});
    sceneTransitionScopeBox.setBounds({});
    sceneTransitionContourHeaderLabel.setBounds({});
    sceneTransitionContourBox.setBounds({});
    sceneTransitionConditionHeaderLabel.setBounds({});
    sceneTransitionConditionBox.setBounds({});
    sceneTransitionEndSampleHeaderLabel.setBounds({});
    sceneTransitionEndSampleBox.setBounds({});
    sceneTransitionEndSampleFolderButton.setBounds({});

    auto transitionDetailRow = settingsBounds.removeFromTop(controlHeight);
    transitionDetailRow.setX(sceneContentX);
    transitionDetailRow.setWidth(sceneContentWidth);
    layoutFillField(takeFillCell(transitionDetailRow), sceneTransitionScopeHeaderLabel, sceneTransitionScopeBox, 38);
    layoutFillField(takeFillCell(transitionDetailRow), sceneTransitionContourHeaderLabel, sceneTransitionContourBox, 48);
    layoutFillField(takeFillCell(transitionDetailRow), sceneTransitionConditionHeaderLabel, sceneTransitionConditionBox, 36);
    auto endSampleCell = takeFillCell(transitionDetailRow, true);
    sceneTransitionEndSampleHeaderLabel.setBounds(endSampleCell.removeFromLeft(24));
    endSampleCell.removeFromLeft(4);
    const int folderButtonWidth = juce::jlimit(54, 84, juce::jmax(54, endSampleCell.getWidth() / 3));
    sceneTransitionEndSampleFolderButton.setBounds(endSampleCell.removeFromRight(folderButtonWidth));
    endSampleCell.removeFromRight(4);
    sceneTransitionEndSampleBox.setBounds(endSampleCell);

    settingsBounds.removeFromTop(4);

    auto summaryRow = settingsBounds.removeFromTop(controlHeight);
    summaryRow.setX(sceneContentX);
    summaryRow.setWidth(sceneContentWidth);
    const int summaryGap = 6;
    const int summaryWidth = juce::jmax(0, (summaryRow.getWidth() - summaryGap) / 2);
    sceneTransitionSummaryLabel.setBounds(summaryRow.removeFromLeft(summaryWidth));
    summaryRow.removeFromLeft(summaryGap);
    sceneTransitionMetaLabel.setBounds(summaryRow);

    sceneAdvanceSummaryLabel.setBounds({});

    bounds.removeFromTop(4);

    auto recorderRow = bounds.removeFromTop(22);
    const int clipGap = 4;
    const int reenableWidth = compactButtonWidthForText("Re-enable", 86, 100);
    const int recWidth = compactButtonWidthForText("Rec", 44, 50);
    const int modPageLabelWidth = 54;
    const int modPageWidth = comboWidthForTexts({"Step Motion", "Main Mod"}, 84, 106);
    sceneRecorderTitleLabel.setBounds({});
    sceneReenableAutomationButton.setBounds(recorderRow.removeFromRight(reenableWidth));
    recorderRow.removeFromRight(10);
    sceneRecordButton.setBounds(recorderRow.removeFromLeft(recWidth));
    recorderRow.removeFromLeft(clipGap);
    sceneCaptureButton.setBounds(recorderRow.removeFromLeft(54));
    recorderRow.removeFromLeft(clipGap);
    sceneGridToggleButton.setBounds(recorderRow.removeFromLeft(50));
    recorderRow.removeFromLeft(clipGap);
    sceneGridDivisionBox.setBounds(recorderRow.removeFromLeft(58));
    recorderRow.removeFromLeft(clipGap + 2);
    sceneZoomBox.setBounds(recorderRow.removeFromLeft(58));
    recorderRow.removeFromLeft(clipGap);
    sceneFollowButton.setBounds(recorderRow.removeFromLeft(60));
    recorderRow.removeFromLeft(clipGap);
    sceneModPageLabel.setBounds(recorderRow.removeFromLeft(modPageLabelWidth));
    recorderRow.removeFromLeft(clipGap);
    sceneModPageBox.setBounds(recorderRow.removeFromLeft(modPageWidth));
    sceneMotionEditButton.setBounds({});
    sceneStatusLabel.setBounds({});
    sceneClearButton.setBounds({});
    sceneDeleteButton.setBounds({});
    sceneClearTriggersButton.setBounds({});
    sceneClearControlsButton.setBounds({});

    bounds.removeFromTop(3);
    auto detailRow = bounds.removeFromTop(15);
    sceneStatusLabel.setBounds(detailRow.removeFromRight(86));
    detailRow.removeFromRight(10);
    sceneDetailLabel.setBounds(detailRow);
    bounds.removeFromTop(2);
    sceneSelectionLabel.setBounds({});

    sceneViewport.setBounds(bounds);
    updateSceneTimelineContentSize();

    sceneLegacyModBackdrop.setBounds(sceneViewport.getBounds());
    sceneLegacyModBackdrop.setVisible(sceneLegacyModEditorVisible);

    if (sceneLegacyModEditor != nullptr)
    {
        const auto editorBounds = sceneViewport.getBounds().reduced(20);
        const int editorWidth = juce::jlimit(320, 520, editorBounds.getWidth());
        const int preferredEditorHeight = sceneLegacyModEditor->getPreferredEmbeddedSceneHeight();
        const int editorHeight = juce::jmin(editorBounds.getHeight(),
                                            juce::jmax(220, preferredEditorHeight));
        const auto centered = juce::Rectangle<int>(editorWidth, editorHeight)
            .withCentre(editorBounds.getCentre())
            .withBottomY(editorBounds.getBottom());
        sceneLegacyModEditor->setBounds(centered);
        sceneLegacyModCloseButton.setBounds(centered.getRight() - 60, centered.getY() + 6, 52, 20);

        auto selectionToolsRow = sceneLegacyModEditor->getEmbeddedSceneSelectionToolsBounds();
        auto overlayToolsRow = sceneLegacyModEditor->getEmbeddedSceneOverlayToolsBounds();
        if (!selectionToolsRow.isEmpty())
        {
            sceneDuplicateButton.setBounds(selectionToolsRow.removeFromLeft(44));
            selectionToolsRow.removeFromLeft(4);
            sceneNudgeLeftButton.setBounds(selectionToolsRow.removeFromLeft(38));
            selectionToolsRow.removeFromLeft(4);
            sceneNudgeRightButton.setBounds(selectionToolsRow.removeFromLeft(38));
            selectionToolsRow.removeFromLeft(4);
            sceneQuantizeButton.setBounds(selectionToolsRow.removeFromLeft(44));
        }
        else
        {
            sceneDuplicateButton.setBounds({});
            sceneNudgeLeftButton.setBounds({});
            sceneNudgeRightButton.setBounds({});
            sceneQuantizeButton.setBounds({});
        }

        if (!overlayToolsRow.isEmpty())
        {
            sceneLaneOverlayButton.setBounds(overlayToolsRow.removeFromLeft(48));
            overlayToolsRow.removeFromLeft(4);
            sceneExpandAllLanesButton.setBounds(overlayToolsRow.removeFromLeft(44));
            overlayToolsRow.removeFromLeft(4);
            sceneCollapseAllLanesButton.setBounds(overlayToolsRow.removeFromLeft(44));
        }
        else
        {
            sceneLaneOverlayButton.setBounds({});
            sceneExpandAllLanesButton.setBounds({});
            sceneCollapseAllLanesButton.setBounds({});
        }

        sceneLegacyModEditor->setVisible(sceneLegacyModEditorVisible);
        sceneLegacyModCloseButton.setVisible(sceneLegacyModEditorVisible);
        if (sceneLegacyModEditorVisible)
        {
            sceneLegacyModBackdrop.toFront(false);
            sceneLegacyModEditor->toFront(true);
            sceneLegacyModCloseButton.toFront(true);
        }
    }

    sceneEndSampleEditorBackdrop.setBounds(sceneViewport.getBounds());
    sceneEndSampleEditorBackdrop.setVisible(sceneEndSampleEditorVisible);

    if (sceneEndSampleEditor != nullptr)
    {
        const auto editorBounds = sceneViewport.getBounds().reduced(22);
        const int editorWidth = juce::jlimit(360, 520, editorBounds.getWidth());
        const int editorHeight = juce::jlimit(320, 424, editorBounds.getHeight());
        const auto centered = juce::Rectangle<int>(editorWidth, editorHeight)
            .withCentre(editorBounds.getCentre());
        sceneEndSampleEditor->setBounds(centered);
        sceneEndSampleEditor->setVisible(sceneEndSampleEditorVisible);
        if (sceneEndSampleEditorVisible)
        {
            sceneEndSampleEditorBackdrop.toFront(false);
            sceneEndSampleEditor->toFront(true);
        }
    }
}

void SceneControlPanel::updateSceneEditorState(double beat)
{
    const int sceneSlot = getFocusedSceneSlot();
    const auto sceneWatcher = processor.getSceneWatcherState();
    const int activeSceneSlot = sceneWatcher.activeSceneSlot;
    const int queuedSceneSlot = sceneWatcher.queuedSceneSlot;
    const bool focusedIsActive = sceneSlot == activeSceneSlot;
    const bool focusedIsQueued = sceneSlot == queuedSceneSlot;
    const double clipLengthBeats = processor.getScenePerformanceClipLengthBeats(sceneSlot);
    const double resolvedLengthBeats = processor.getResolvedSceneLengthBeats(sceneSlot);
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const bool editingEnabled = !processor.isScenePerformanceRecording();
    const bool clipboardReady = processor.hasScenePerformanceClipboard();
    const int clipboardSourceSlot = processor.getScenePerformanceClipboardSourceSlot();
    const juce::String clipboardSummary = clipboardReady
        ? (juce::String("Clipboard S") + juce::String(clipboardSourceSlot + 1))
        : juce::String();

    if (sceneEditorState.selectedSceneSlot != sceneSlot)
    {
        sceneEditorState.selectedSceneSlot = sceneSlot;
        sceneEditorState.selectedStripIndex = 0;
        clearSceneSelection();
        sceneEditorState.dragActive = false;
        sceneEditorState.dragEventIndex = -1;
        sceneEditorState.dragBaseEvents.clear();
        sceneEditorState.dragTriggerMoveTime = true;
        sceneEditorState.dragTriggerMoveOffset = true;
        sceneEditorState.drawActive = false;
        sceneEditorState.drawTriggerLane = false;
        sceneEditorState.drawStripIndex = -1;
        sceneEditorState.drawLaneIndex = -1;
        sceneEditorState.drawLastBeat = 0.0;
        sceneEditorState.drawLastValue = 0.0f;
        sceneEditorState.drawHasLastPoint = false;
        sceneEditorState.clickLinePending = false;
        sceneEditorState.clickLineStripIndex = -1;
        sceneEditorState.clickLineLaneIndex = -1;
        sceneEditorState.clickLineBeat = 0.0;
        sceneEditorState.clickLineValue = 0.0f;
        sceneEditorState.eraseActive = false;
        sceneEditorState.eraseTriggerLane = false;
        sceneEditorState.eraseStripIndex = -1;
        sceneEditorState.eraseLaneIndex = -1;
        sceneEditorState.marqueeActive = false;
        sceneEditorState.marqueeRect = {};
        sceneEditorState.hoverActive = false;
        sceneEditorState.hoverTriggerLane = false;
        sceneEditorState.hoverStripIndex = -1;
        sceneEditorState.hoverLaneIndex = -1;
        sceneEditorState.hoverEventIndex = -1;
        sceneEditorState.hoverTriggerMoveTime = true;
        sceneEditorState.hoverTriggerMoveOffset = true;
        sceneEditorState.hoverBeat = 0.0;
        sceneTimelineCanvas.setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    sceneEditorState.events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    sceneEditorState.triggerEventCount = 0;
    sceneEditorState.controlEventCount = 0;
    sceneEditorState.transportProgress = -1.0f;
    sceneEditorState.transportRecording = false;

    if (sceneEditorState.selectedEventIndex >= static_cast<int>(sceneEditorState.events.size()))
        sceneEditorState.selectedEventIndex = -1;
    setSceneSelectionIndices(sceneEditorState.selectedEventIndices, sceneEditorState.selectedEventIndex);

    for (const auto& event : sceneEditorState.events)
    {
        if (event.type == ScenePerformanceEventType::ControlPoint)
            ++sceneEditorState.controlEventCount;
        else
            ++sceneEditorState.triggerEventCount;
    }

    const bool hasEngine = (processor.getAudioEngine() != nullptr);
    sceneEditorState.selectedStripIndex = juce::jlimit(0,
                                                       juce::jmax(0, getVisibleSceneStripCount() - 1),
                                                       sceneEditorState.selectedStripIndex);
    auto& mutableProcessor = processor;
    for (int stripIndex = 0; stripIndex < getVisibleSceneStripCount(); ++stripIndex)
    {
        for (int lane = 0; lane < SceneAutomationLaneCount; ++lane)
        {
            auto& targetBox = sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)];
            const auto selectedTarget = hasEngine
                ? sceneDisplayedModTargetForLane(mutableProcessor, stripIndex, lane)
                : ModernAudioEngine::ModTarget::None;
            targetBox.setEnabled(hasEngine && sceneLaneCanShowMotionTargetSelector(mutableProcessor, stripIndex, lane));
            targetBox.setSelectedId(performanceTargetToComboId(selectedTarget),
                                    juce::dontSendNotification);
        }
    }

    sceneRecorderTitleLabel.setText("CLIP", juce::dontSendNotification);
    sceneCaptureButton.setToggleState(sceneDrawModeEnabled, juce::dontSendNotification);
    sceneCaptureButton.setButtonText("Draw");
    const auto sceneModPageMode = processor.getSceneModPageMode();
    const int preferredStripIndex = sceneLegacyModEditorVisible && sceneLegacyModStripIndex >= 0
        ? sceneLegacyModStripIndex
        : (sceneEditorState.hoverStripIndex >= 0 ? sceneEditorState.hoverStripIndex : sceneEditorState.selectedStripIndex);
    const int preferredLaneIndex = sceneEditorState.hoverLaneIndex >= 0 ? sceneEditorState.hoverLaneIndex : 0;
    const int preferredSlotIndex = sceneLegacyModEditorVisible && sceneLegacyModSlotIndex >= 0
        ? sceneLegacyModSlotIndex
        : preferredLegacyModEditorSlotForLane(preferredStripIndex, preferredLaneIndex);
    const auto preferredTarget = (hasEngine && preferredSlotIndex >= 0)
        ? processor.getSceneMotionTargetForSlot(preferredStripIndex, preferredSlotIndex)
        : ModernAudioEngine::ModTarget::None;
    const bool sceneModPageActive = processor.isSceneModeEnabled()
        && processor.isControlModeActive()
        && processor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation;
    if (sceneModPageActive)
    {
        if (sceneModPageMode == MlrVSTAudioProcessor::SceneModPageMode::StepMotion)
        {
            if (!sceneLegacyModEditorVisible || sceneLegacyModSlotIndex < 0)
                openLegacyModEditorForLane(preferredStripIndex, preferredLaneIndex);
        }
        else if (sceneLegacyModEditorVisible)
        {
            closeLegacyModEditor();
        }
    }
    else if (sceneLegacyModEditorVisible && sceneLegacyModSlotIndex < 0)
    {
        closeLegacyModEditor();
    }
    sceneMotionEditButton.setEnabled(false);
    sceneMotionEditButton.setToggleState(sceneLegacyModEditorVisible, juce::dontSendNotification);
    sceneMotionEditButton.setButtonText(sceneLegacyModEditorVisible
                                            ? "Close"
                                            : (preferredTarget == ModernAudioEngine::ModTarget::None ? "Assign"
                                                                                                     : ("Mod M" + juce::String(preferredSlotIndex + 1))));
    sceneMotionEditButton.setTooltip(sceneLegacyModEditorVisible
                                         ? "Close the embedded scene motion editor."
                                         : (preferredTarget == ModernAudioEngine::ModTarget::None
                                                ? "Open the scene motion editor and assign a target for the hovered strip."
                                                : "Open the classic mod-step editor for the hovered or selected scene motion lane. Depth, Rate, Clock, and Length live inside the editor."));
    sceneMotionEditButton.setVisible(false);
    sceneLegacyModCloseButton.setTooltip(sceneModPageMode == MlrVSTAudioProcessor::SceneModPageMode::StepMotion
                                             ? "Close the scene motion editor and leave the monome Mod page."
                                             : "Close the main modulation editor and leave the monome Mod page.");
    const bool recordingThisScene = processor.isScenePerformanceRecording()
        && processor.getScenePerformanceRecordingSceneSlot() == sceneSlot;
    const bool hasAutomationOverrides = focusedIsActive && processor.hasAnyActiveSceneAutomationOverrides();
    const bool blinkOn = hasEngine && processor.getAudioEngine()->shouldBlinkRecordLED();
    const bool overdubbingThisScene = recordingThisScene && processor.isScenePerformanceOverdubbing();
    sceneRecordButton.setToggleState(recordingThisScene, juce::dontSendNotification);
    sceneRecordButton.setButtonText(recordingThisScene
                                        ? (overdubbingThisScene ? "Dub" : "Rec")
                                        : "Rec");
    sceneRecordButton.setEnabled(focusedIsActive);
    sceneRecordButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff374048));
    sceneRecordButton.setColour(juce::TextButton::buttonOnColourId,
                                recordingThisScene
                                    ? (overdubbingThisScene
                                           ? (blinkOn ? juce::Colour(0xffdf9152) : juce::Colour(0xff7a4a31))
                                           : (blinkOn ? juce::Colour(0xffd45b5b) : juce::Colour(0xff6d3434)))
                                    : juce::Colour(0xff374048));
    sceneRecordButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff2f4f6));
    sceneRecordButton.setColour(juce::TextButton::textColourOnId,
                                recordingThisScene ? juce::Colour(0xfffcfcfd) : juce::Colour(0xfff2f4f6));
    sceneReenableAutomationButton.setEnabled(hasAutomationOverrides);
    sceneReenableAutomationButton.setToggleState(hasAutomationOverrides, juce::dontSendNotification);
    sceneReenableAutomationButton.setTooltip(hasAutomationOverrides
                                                 ? "Greyed-out scene automation lanes are currently overridden by live touches. Click to re-enable their written values. Step motion lanes are unaffected."
                                                 : "Touch a recorded scene control live to temporarily grey it out, then click here to re-enable its written automation. Step motion lanes are unaffected.");
    sceneReenableAutomationButton.setColour(juce::TextButton::buttonColourId,
                                            hasAutomationOverrides ? kAccent.withAlpha(0.92f)
                                                                   : juce::Colour(0xff3b4146));
    sceneReenableAutomationButton.setColour(juce::TextButton::buttonOnColourId,
                                            hasAutomationOverrides ? kAccent.brighter(0.12f)
                                                                   : juce::Colour(0xff4a5258));
    sceneReenableAutomationButton.setColour(juce::TextButton::textColourOffId,
                                            hasAutomationOverrides ? juce::Colour(0xff141414)
                                                                   : kTextPrimary);
    sceneReenableAutomationButton.setColour(juce::TextButton::textColourOnId,
                                            hasAutomationOverrides ? juce::Colour(0xff101010)
                                                                   : juce::Colour(0xfff5f5f5));
    const int selectedCount = static_cast<int>(sceneEditorState.selectedEventIndices.size());
    sceneDeleteButton.setEnabled(false);
    sceneClearTriggersButton.setEnabled(false);
    sceneClearControlsButton.setEnabled(false);
    sceneClearButton.setEnabled(editingEnabled);
    sceneDuplicateButton.setEnabled(editingEnabled && selectedCount > 0);
    sceneNudgeLeftButton.setEnabled(editingEnabled && selectedCount > 0);
    sceneNudgeRightButton.setEnabled(editingEnabled && selectedCount > 0);
    sceneQuantizeButton.setEnabled(editingEnabled && selectedCount > 0);
    sceneClearButton.setTooltip("Clear scene S" + juce::String(sceneSlot + 1)
                                + " clip data and reset all scene motion lanes.");
    sceneDeleteButton.setTooltip("Delete the selected scene event(s) from scene S" + juce::String(sceneSlot + 1) + ".");
    sceneRecordButton.setTooltip(
        focusedIsActive
            ? "Single-click records the current scene cycle. Double-click overdubs another pass."
            : (juce::String("Recording follows the active scene. S") + juce::String(activeSceneSlot + 1)
               + (focusedIsQueued ? " is about to switch." : " is currently live.")));

    const juce::String eventSummary = (sceneEditorState.triggerEventCount > 0 || sceneEditorState.controlEventCount > 0)
        ? (juce::String(sceneEditorState.triggerEventCount) + " trig • "
           + juce::String(sceneEditorState.controlEventCount) + " ctrl")
        : juce::String("No events recorded");
    juce::ignoreUnused(editingEnabled, hasAutomationOverrides, clipboardReady, clipboardSummary);
    sceneSelectionLabel.setVisible(false);
    sceneSelectionLabel.setText({}, juce::dontSendNotification);
    sceneSelectionLabel.setTooltip({});

    if (recordingThisScene)
    {
        const double startBeat = processor.getScenePerformanceRecordingStartBeat();
        const double endBeat = processor.getScenePerformanceRecordingEndBeat();
        const double beatsLeft = juce::jmax(0.0, endBeat - beat);
        const double recordingProgress = processor.getScenePerformanceRecordingProgress(beat);
        if (recordingProgress >= 0.0)
            sceneEditorState.transportProgress = static_cast<float>(recordingProgress);
        else if (endBeat > startBeat)
            sceneEditorState.transportProgress = static_cast<float>(juce::jlimit(0.0, 1.0, (beat - startBeat) / juce::jmax(1.0, lengthBeats)));
        sceneEditorState.transportRecording = true;
        sceneStatusLabel.setText(processor.isScenePerformanceOverdubbing() ? "DUB" : "REC",
                                 juce::dontSendNotification);
        sceneStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd46b62));
        juce::String detail = "S" + juce::String(sceneSlot + 1)
            + " • " + juce::String(lengthBeats, 2) + " beats"
            + " • " + eventSummary
            + " • " + juce::String(beatsLeft, 2) + " beats left";
        sceneDetailLabel.setText(detail,
                                 juce::dontSendNotification);
    }
    else if (processor.hasScenePerformanceClip(sceneSlot))
    {
        if (focusedIsActive)
        {
            const double clipProgress = processor.getScenePerformancePlaybackProgress(sceneSlot, beat);
            if (clipProgress >= 0.0)
                sceneEditorState.transportProgress = static_cast<float>(
                    juce::jlimit(0.0, 1.0, clipProgress * (clipLengthBeats / juce::jmax(1.0, lengthBeats))));
            sceneStatusLabel.setText("LIVE", juce::dontSendNotification);
            sceneStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff76be7e));
            juce::String detail = "S" + juce::String(sceneSlot + 1) + " • " + juce::String(resolvedLengthBeats, 2) + " beats";
            if (std::abs(clipLengthBeats - resolvedLengthBeats) > 1.0e-6)
                detail << " • Clip " << juce::String(clipLengthBeats, 2) << " beats";
            detail << " • " << eventSummary;
            sceneDetailLabel.setText(detail,
                                     juce::dontSendNotification);
        }
        else
        {
            sceneStatusLabel.setText(focusedIsQueued ? "QUEUED" : "READY", juce::dontSendNotification);
            sceneStatusLabel.setColour(juce::Label::textColourId,
                                       focusedIsQueued ? juce::Colour(0xfff2b544) : kTextPrimary);
            sceneDetailLabel.setText(juce::String("S") + juce::String(sceneSlot + 1)
                                         + " • " + juce::String(resolvedLengthBeats, 2) + " beats"
                                         + " • " + eventSummary,
                                     juce::dontSendNotification);
        }
    }
    else
    {
        sceneStatusLabel.setText("EMPTY", juce::dontSendNotification);
        sceneStatusLabel.setColour(juce::Label::textColourId, kTextMuted);
        sceneDetailLabel.setText(juce::String("S") + juce::String(sceneSlot + 1)
                                     + " • " + juce::String(resolvedLengthBeats, 2) + " beats"
                                     + " • No events recorded",
                                 juce::dontSendNotification);
    }

    sceneDetailLabel.setTooltip(sceneDetailLabel.getText());
    const auto statusAccent = sceneStatusLabel.findColour(juce::Label::textColourId);
    sceneStatusLabel.setColour(juce::Label::backgroundColourId, statusAccent.withAlpha(0.16f));
    sceneStatusLabel.setColour(juce::Label::outlineColourId, statusAccent.withAlpha(0.34f));
}

bool SceneControlPanel::isLegacyModEditorAvailableForLane(int stripIndex, int laneIndex) const
{
    return preferredLegacyModEditorSlotForLane(stripIndex, laneIndex) >= 0;
}

int SceneControlPanel::preferredLegacyModEditorSlotForLane(int stripIndex, int laneIndex) const
{
    if (stripIndex < 0 || stripIndex >= getVisibleSceneStripCount())
        return -1;

    auto& mutableProcessor = const_cast<MlrVSTAudioProcessor&>(processor);
    return scenePreferredModEditorSlotForLane(mutableProcessor, stripIndex, laneIndex);
}

void SceneControlPanel::openLegacyModEditorForMainMod()
{
    if (sceneLegacyModEditor == nullptr)
        return;

    closeSceneTransitionEndSampleEditor();

    const int fallbackStripIndex = juce::jlimit(0,
                                                juce::jmax(0, getVisibleSceneStripCount() - 1),
                                                sceneEditorState.selectedStripIndex);
    sceneLegacyModStripIndex = juce::jlimit(0,
                                            getVisibleSceneStripCount() - 1,
                                            sceneEditorState.hoverStripIndex >= 0 ? sceneEditorState.hoverStripIndex : fallbackStripIndex);
    sceneLegacyModSlotIndex = -1;
    sceneEditorState.dragActive = false;
    sceneEditorState.dragEventIndex = -1;
    sceneEditorState.dragBaseEvents.clear();
    sceneEditorState.drawActive = false;
    sceneEditorState.drawTriggerLane = false;
    sceneEditorState.drawStripIndex = -1;
    sceneEditorState.drawLaneIndex = -1;
    sceneEditorState.drawHasLastPoint = false;
    sceneEditorState.eraseActive = false;
    sceneEditorState.eraseTriggerLane = false;
    sceneEditorState.eraseStripIndex = -1;
    sceneEditorState.eraseLaneIndex = -1;
    sceneEditorState.marqueeActive = false;
    sceneEditorState.marqueeRect = {};
    sceneEditorState.stepPatternPaintActive = false;
    sceneEditorState.stepPatternPaintStripIndex = -1;
    sceneEditorState.stepPatternPaintLastStep = -1;
    sceneEditorState.stepPatternPaintEnabled = false;
    sceneLegacyModEditorVisible = true;
    processor.setSceneStepMotionEditorOpen(false);
    sceneEditorState.selectedStripIndex = sceneLegacyModStripIndex;
    sceneLegacyModEditor->clearPinnedStripAndSlot();
    resized();
    sceneTimelineCanvas.repaint();
}

void SceneControlPanel::openLegacyModEditorForSlot(int stripIndex, int slot)
{
    if (sceneLegacyModEditor == nullptr)
        return;

    closeSceneTransitionEndSampleEditor();

    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    const int safeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, slot);
    sceneLegacyModStripIndex = safeStripIndex;
    sceneLegacyModSlotIndex = safeSlot;
    sceneEditorState.dragActive = false;
    sceneEditorState.dragEventIndex = -1;
    sceneEditorState.dragBaseEvents.clear();
    sceneEditorState.drawActive = false;
    sceneEditorState.drawTriggerLane = false;
    sceneEditorState.drawStripIndex = -1;
    sceneEditorState.drawLaneIndex = -1;
    sceneEditorState.drawHasLastPoint = false;
    sceneEditorState.eraseActive = false;
    sceneEditorState.eraseTriggerLane = false;
    sceneEditorState.eraseStripIndex = -1;
    sceneEditorState.eraseLaneIndex = -1;
    sceneEditorState.marqueeActive = false;
    sceneEditorState.marqueeRect = {};
    sceneEditorState.stepPatternPaintActive = false;
    sceneEditorState.stepPatternPaintStripIndex = -1;
    sceneEditorState.stepPatternPaintLastStep = -1;
    sceneEditorState.stepPatternPaintEnabled = false;
    sceneLegacyModEditorVisible = true;
    processor.setSceneStepMotionEditorOpen(true);
    sceneEditorState.selectedStripIndex = safeStripIndex;
    sceneLegacyModEditor->setPinnedStripAndSlot(
        safeStripIndex,
        safeSlot,
        "Scene Motion  Strip " + juce::String(safeStripIndex + 1) + "  M" + juce::String(safeSlot + 1));
    resized();
    sceneTimelineCanvas.repaint();
}

void SceneControlPanel::openLegacyModEditorForLane(int stripIndex, int laneIndex)
{
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    auto& mutableProcessor = processor;
    int slot = sceneAssignedModSlotForLane(mutableProcessor, safeStripIndex, laneIndex);
    if (slot < 0)
    {
        const auto desiredTarget = sceneDisplayedModTargetForLane(mutableProcessor, safeStripIndex, laneIndex);
        slot = preferredLegacyModEditorSlotForLane(safeStripIndex, laneIndex);
        if (slot >= 0 && desiredTarget != ModernAudioEngine::ModTarget::None)
        {
            processor.setSceneMotionTargetForSlot(safeStripIndex, slot, desiredTarget);
            slot = sceneAssignedModSlotForLane(mutableProcessor, safeStripIndex, laneIndex);
        }
    }
    if (slot < 0)
        return;

    openLegacyModEditorForSlot(safeStripIndex, slot);
}

void SceneControlPanel::closeLegacyModEditor()
{
    sceneLegacyModEditorVisible = false;
    processor.setSceneStepMotionEditorOpen(false);
    sceneLegacyModStripIndex = -1;
    sceneLegacyModSlotIndex = -1;
    if (sceneLegacyModEditor != nullptr)
    {
        sceneLegacyModEditor->clearPinnedStripAndSlot();
        sceneLegacyModEditor->setVisible(false);
    }
    sceneLegacyModBackdrop.setVisible(false);
    sceneLegacyModCloseButton.setVisible(false);
    sceneTimelineCanvas.repaint();
}

void SceneControlPanel::openSceneTransitionEndSampleEditor(int stepIndex)
{
    if (sceneEndSampleEditor == nullptr)
        return;

    const int chainLength = processor.getSceneChainLength();
    if (stepIndex < 0 || stepIndex >= chainLength)
        return;

    closeLegacyModEditor();
    selectedSceneChainStep = stepIndex;
    sceneEndSampleEditorVisible = true;
    sceneEndSampleEditorStep = stepIndex;
    sceneEndSampleEditor->setPinnedStep(stepIndex, sceneTransitionEndSampleEditorContext(processor, stepIndex));
    resized();
    sceneChainCanvas.repaint();
}

void SceneControlPanel::closeSceneTransitionEndSampleEditor()
{
    sceneEndSampleEditorVisible = false;
    sceneEndSampleEditorStep = -1;
    if (sceneEndSampleEditor != nullptr)
    {
        sceneEndSampleEditor->clearPinnedStep();
        sceneEndSampleEditor->setVisible(false);
    }
    sceneEndSampleEditorBackdrop.setVisible(false);
    sceneChainCanvas.repaint();
}

void SceneControlPanel::refreshFromProcessor()
{
    sceneGridEnabled = processor.getSceneEditorGridEnabled();
    sceneGridDivision = processor.getSceneEditorGridDivision();
    sceneZoomFactor = processor.getSceneEditorZoomFactor();
    sceneDrawModeEnabled = processor.getSceneEditorDrawModeEnabled();
    sceneFollowPlayheadEnabled = processor.getSceneEditorFollowPlayheadEnabled();
    sceneLaneOverlayEnabled = processor.getSceneEditorLaneOverlaysEnabled();
    for (int stripIndex = 0; stripIndex < getVisibleSceneStripCount(); ++stripIndex)
    {
        stripAutomationExpanded[static_cast<size_t>(stripIndex)] =
            processor.getSceneEditorStripAutomationExpanded(stripIndex);
        stripHeightExpanded[static_cast<size_t>(stripIndex)] =
            processor.getSceneEditorStripHeightExpanded(stripIndex);
    }

    sceneModeToggle.setToggleState(processor.isSceneModeEnabled(), juce::dontSendNotification);
    sceneChangeModeBox.setSelectedId(static_cast<int>(processor.getSceneRecallMode()) + 1,
                                     juce::dontSendNotification);
    sceneGridToggleButton.setToggleState(sceneGridEnabled, juce::dontSendNotification);
    sceneGridDivisionBox.setSelectedId(sceneGridDivision, juce::dontSendNotification);
    sceneGridDivisionBox.setEnabled(sceneGridEnabled);
    sceneZoomBox.setSelectedId(sceneZoomFactor, juce::dontSendNotification);
    sceneCaptureButton.setToggleState(sceneDrawModeEnabled, juce::dontSendNotification);
    sceneCaptureButton.setButtonText("Draw");
    sceneFollowButton.setToggleState(sceneFollowPlayheadEnabled, juce::dontSendNotification);
    sceneReenableAutomationButton.setToggleState(processor.hasAnyActiveSceneAutomationOverrides(),
                                                 juce::dontSendNotification);
    sceneLaneOverlayButton.setToggleState(sceneLaneOverlayEnabled, juce::dontSendNotification);
    sceneMotionEditButton.setToggleState(sceneLegacyModEditorVisible, juce::dontSendNotification);
    sceneCaptureButton.setTooltip(sceneDrawModeEnabled
                                      ? "Draw mode is on. Drag in trigger lanes to paint hits and automation lanes to paint quantized curves. Option-click resets automation points to the lane default, Ctrl/Cmd-click defaults a whole automation lane, and Option-drag snaps bipolar lanes to center."
                                      : "Draw mode is off. Click automation points to place anchors and connect line segments between clicks. Enable Draw to paint triggers and automation directly into the clip grid. Option-click resets automation points to the lane default, Ctrl/Cmd-click defaults a whole automation lane, and Option-drag snaps bipolar lanes to center.");
    const int chainLength = processor.getSceneChainLength();
    if (selectedSceneChainStep >= chainLength)
        selectedSceneChainStep = -1;
    const bool sceneSlotClipboardReady = processor.hasSceneSlotClipboard();
    const int sceneSlotClipboardSourceSlot = processor.getSceneSlotClipboardSourceSlot();
    juce::String chainHint = chainLength > 0
        ? juce::String("Scenes live up top, playback rules sit underneath, and the rail shapes order plus fills.")
        : juce::String("Scenes live up top. Click an empty rail slot to add the focused scene.");
    if (chainLength > 0)
        chainHint << "  " << processor.getSceneSequenceSummaryText() << ".";
    juce::String chainHintTooltip = "Scene buttons focus and launch scenes. Playback controls decide when the chain moves next. Click a chain step to focus it without launching, drag steps to reorder, wheel steps to change repeats, and use connector bubbles or the loop clamp to shape Chain Fills. Drag a bubble horizontally for Lead and vertically for Blend. Plain wheel changes Style, Shift-wheel changes Lead, Cmd/Ctrl-wheel changes Blend, Alt-wheel changes Type, Shift+Cmd/Ctrl-wheel changes Scope, Shift+Alt-wheel changes Contour, Cmd/Ctrl+Alt-wheel changes When, and double-click toggles Shorten Scene vs Ride Over End. Chains auto-loop.";
    if (chainLength > 0)
        chainHintTooltip = processor.getSceneSequenceSummaryText() + ".  " + chainHintTooltip;
    hintLabel.setText(chainHint, juce::dontSendNotification);
    hintLabel.setTooltip(chainHintTooltip);
    hintLabel.setColour(juce::Label::textColourId,
                        processor.isSceneChainPlaybackActive() ? kAccent : kTextMuted);
    if (selectedSceneActionSlot < 0 || selectedSceneActionSlot >= MlrVSTAudioProcessor::SceneSlots)
        selectedSceneActionSlot = processor.getFocusedSceneSlot();
    const int focusedSceneSlot = getFocusedSceneSlot();
    const bool focusedSceneHasStoredData = processor.getSceneClipSlotState(focusedSceneSlot).hasStoredContent;
    const auto sceneModPageMode = processor.getSceneModPageMode();
    if (sceneTimingLayoutFocusedSlot != focusedSceneSlot)
        resized();
    sceneSceneCaptureButton.setEnabled(processor.isSceneModeEnabled());
    sceneSceneCopyButton.setEnabled(processor.isSceneModeEnabled() && focusedSceneHasStoredData);
    sceneScenePasteButton.setEnabled(processor.isSceneModeEnabled() && sceneSlotClipboardReady);
    sceneModPageBox.setSelectedId(static_cast<int>(sceneModPageMode) + 1, juce::dontSendNotification);
    sceneModPageLabel.setTooltip("Choose what the monome Mod page edits while scene mode is active.");
    sceneModPageBox.setTooltip(sceneModPageMode == MlrVSTAudioProcessor::SceneModPageMode::StepMotion
                                   ? "Step Motion: the monome Mod page opens the scene motion step editor and writes per-scene motion."
                                   : "Main Mod: the monome Mod page stays on the main scene automation lanes instead of opening the legacy modulation editor.");
    const bool sceneChainPlaybackActive = processor.isSceneChainPlaybackActive();
    sceneChainPlayButton.setEnabled(chainLength >= 2);
    sceneChainPlayButton.setToggleState(sceneChainPlaybackActive, juce::dontSendNotification);
    sceneChainPlayButton.setButtonText(sceneChainPlaybackActive ? "STOP" : "PLAY");
    sceneChainPlayButton.setColour(juce::TextButton::buttonColourId,
                                   chainLength >= 2 ? juce::Colour(0xff315649)
                                                    : juce::Colour(0xff2b3137));
    sceneChainPlayButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffc9eb74));
    sceneChainPlayButton.setColour(juce::TextButton::textColourOffId,
                                   chainLength >= 2 ? juce::Colour(0xfff2f6f3)
                                                    : kTextMuted.withAlpha(0.84f));
    sceneChainPlayButton.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff10150f));
    sceneChainClearButton.setEnabled(processor.isSceneModeEnabled());
    const int focusedLengthCount = processor.getSceneLengthCount(focusedSceneSlot);
    const auto focusedLengthMode = processor.getSceneLengthMode(focusedSceneSlot);
    const int focusedStep = getFocusedSceneChainStep() >= 0
        ? getFocusedSceneChainStep()
        : (sceneEndSampleEditorVisible ? sceneEndSampleEditorStep : -1);
    const bool hasFocusedStep = focusedStep >= 0 && focusedStep < chainLength;
    if (sceneEndSampleEditorVisible)
    {
        if (!hasFocusedStep)
            closeSceneTransitionEndSampleEditor();
        else if (sceneEndSampleEditorStep != focusedStep && sceneEndSampleEditor != nullptr)
            sceneEndSampleEditor->setPinnedStep(focusedStep,
                                                sceneTransitionEndSampleEditorContext(processor, focusedStep));
        sceneEndSampleEditorStep = hasFocusedStep ? focusedStep : -1;
    }
    sceneSlotHeaderLabel.setText("Edit S" + juce::String(focusedSceneSlot + 1), juce::dontSendNotification);
    sceneSlotHeaderLabel.setColour(juce::Label::textColourId, sceneSlotUiColour(processor, focusedSceneSlot).brighter(0.12f));
    sceneSlotHeaderLabel.setTooltip("The scene whose Length and Count settings are shown below.");
    sceneAnchorHeaderLabel.setVisible(focusedLengthMode == MlrVSTAudioProcessor::SceneLengthMode::AnchorStrip);

    sceneInsertBeforeButton.setTooltip("Shift later scenes right and capture before scene S"
                                       + juce::String(getFocusedSceneSlot() + 1) + ".");
    sceneInsertAfterButton.setTooltip("Shift later scenes right and capture after scene S"
                                      + juce::String(getFocusedSceneSlot() + 1) + ".");
    sceneInsertAfterButton.setEnabled(getFocusedSceneSlot() < MlrVSTAudioProcessor::SceneSlots - 1);
    sceneDuplicateLengthButton.setEnabled(focusedLengthCount <= (MlrVSTAudioProcessor::MaxSceneManualBars / 2));
    sceneSceneCaptureButton.setTooltip("Save the current live state into focused scene S"
                                       + juce::String(getFocusedSceneSlot() + 1) + ".");
    sceneDuplicateLengthButton.setTooltip(sceneDuplicateLengthButton.isEnabled()
                                              ? ("Duplicate scene S" + juce::String(getFocusedSceneSlot() + 1)
                                                 + " into a second pass and double its length count.")
                                              : ("Scene S" + juce::String(getFocusedSceneSlot() + 1)
                                                 + " is already at the maximum length count for 2x duplication."));
    sceneSceneCopyButton.setTooltip("Copy focused scene S"
                                    + juce::String(getFocusedSceneSlot() + 1)
                                    + (focusedSceneHasStoredData
                                           ? " so it can be pasted into another scene slot."
                                           : " after you save something into it first."));
    sceneScenePasteButton.setTooltip(sceneSlotClipboardReady
                                         ? ("Paste copied scene from S"
                                            + juce::String(sceneSlotClipboardSourceSlot + 1)
                                            + " into focused scene S"
                                            + juce::String(getFocusedSceneSlot() + 1)
                                            + ".")
                                         : "Copy a scene first, then paste it into the focused scene slot.");
    sceneChainPlayButton.setTooltip(chainLength >= 2
                                        ? (processor.isSceneChainPlaybackActive()
                                               ? "Stop chain playback. Top scene buttons still launch manually on the trigger grid, take over from the chain, and clicking a chain step only focuses that scene for editing."
                                               : "Start chain playback from the focused scene if it is in the chain, otherwise from step 1. If that start step is already live, playback keeps running and the chain just takes over.")
                                        : "Chain playback becomes available once the chain has at least two steps.");
    sceneChainClearButton.setTooltip("Reset focused scene S"
                                     + juce::String(getFocusedSceneSlot() + 1)
                                     + " to its default empty state. Alt-click clears the chain.");

    const bool queuedSceneBlinkOn = ((juce::Time::getMillisecondCounter() / 500u) & 1u) == 0u;

    if (sceneTimingLayoutFocusedStep != focusedStep)
        resized();

    sceneTransitionTypeBox.setEnabled(hasFocusedStep);
    sceneTransitionOptionsBox.setEnabled(hasFocusedStep);
    sceneTransitionScopeBox.setEnabled(hasFocusedStep);
    sceneTransitionContourBox.setEnabled(hasFocusedStep);
    sceneTransitionConditionBox.setEnabled(hasFocusedStep);
    sceneTransitionLengthBox.setEnabled(hasFocusedStep);
    sceneTransitionMixSlider.setEnabled(hasFocusedStep);
    sceneTransitionDelaySlider.setEnabled(hasFocusedStep);
    sceneTransitionFilterSlider.setEnabled(hasFocusedStep);
    sceneTransitionChopSlider.setEnabled(hasFocusedStep);
    sceneTransitionSubtractButton.setEnabled(hasFocusedStep);
    sceneTransitionEndSampleBox.setEnabled(hasFocusedStep);
    sceneTransitionEndSampleFolderButton.setEnabled(hasFocusedStep);
    sceneTransitionTypeBox.setSelectedId(hasFocusedStep
                                             ? (static_cast<int>(processor.getSceneChainStepTransitionType(focusedStep)) + 1)
                                             : 0,
                                         juce::dontSendNotification);
    sceneTransitionOptionsBox.setSelectedId(hasFocusedStep
                                                ? (static_cast<int>(processor.getSceneChainStepTransitionOption(focusedStep)) + 1)
                                                : 0,
                                            juce::dontSendNotification);
    sceneTransitionScopeBox.setSelectedId(hasFocusedStep
                                              ? (static_cast<int>(processor.getSceneChainStepTransitionScope(focusedStep)) + 1)
                                              : 0,
                                          juce::dontSendNotification);
    sceneTransitionContourBox.setSelectedId(hasFocusedStep
                                                ? (static_cast<int>(processor.getSceneChainStepTransitionContour(focusedStep)) + 1)
                                                : 0,
                                            juce::dontSendNotification);
    sceneTransitionConditionBox.setSelectedId(hasFocusedStep
                                                  ? (static_cast<int>(processor.getSceneChainStepTransitionCondition(focusedStep)) + 1)
                                                  : 0,
                                              juce::dontSendNotification);
    sceneTransitionLengthBox.setSelectedId(hasFocusedStep
                                               ? sceneTransitionComboIdForValue(
                                                     processor.getSceneChainStepTransitionLengthBeats(focusedStep),
                                                     kSceneTransitionLengthOptions,
                                                     MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats)
                                               : 0,
                                           juce::dontSendNotification);

    const auto focusedTransitionType = hasFocusedStep
        ? processor.getSceneChainStepTransitionType(focusedStep)
        : MlrVSTAudioProcessor::SceneChainTransitionType::None;
    const auto focusedTransitionOption = hasFocusedStep
        ? processor.getSceneChainStepTransitionOption(focusedStep)
        : MlrVSTAudioProcessor::SceneChainTransitionOption::Default;
    const auto focusedTransitionScope = hasFocusedStep
        ? processor.getSceneChainStepTransitionScope(focusedStep)
        : MlrVSTAudioProcessor::SceneChainTransitionScope::All;
    const auto focusedTransitionContour = hasFocusedStep
        ? processor.getSceneChainStepTransitionContour(focusedStep)
        : MlrVSTAudioProcessor::SceneChainTransitionContour::Smooth;
    const auto focusedTransitionCondition = hasFocusedStep
        ? processor.getSceneChainStepTransitionCondition(focusedStep)
        : MlrVSTAudioProcessor::SceneChainTransitionCondition::Always;
    const float focusedTransitionLength = hasFocusedStep
        ? processor.getSceneChainStepTransitionLengthBeats(focusedStep)
        : MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    const bool focusedTransitionSubtractsFromSceneLength = hasFocusedStep
        ? processor.getSceneChainStepTransitionSubtractsFromSceneLength(focusedStep)
        : false;
    const float focusedTransitionIntensity = hasFocusedStep
        ? processor.getSceneChainStepTransitionIntensity(focusedStep)
        : MlrVSTAudioProcessor::DefaultSceneTransitionIntensity;
    const float focusedTransitionDelay = hasFocusedStep
        ? processor.getSceneChainStepTransitionDelayAmount(focusedStep)
        : MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount;
    const float focusedTransitionFilter = hasFocusedStep
        ? processor.getSceneChainStepTransitionFilterAmount(focusedStep)
        : MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount;
    const float focusedTransitionChop = hasFocusedStep
        ? processor.getSceneChainStepTransitionChopAmount(focusedStep)
        : MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount;
    const auto focusedTransitionEndSample = hasFocusedStep
        ? processor.getSceneChainStepTransitionEndSampleFile(focusedStep)
        : juce::File();
    const auto endSampleDirectory = processor.getSceneTransitionEndSampleDirectory();
    const auto transitionColour = hasFocusedStep
        ? sceneChainTransitionColour(focusedTransitionType)
        : juce::Colour(0xff465158);
    auto tintTransitionSlider = [&](juce::Slider& slider, float value)
    {
        slider.setColour(juce::Slider::trackColourId, transitionColour.withAlpha(hasFocusedStep ? 0.84f : 0.26f));
        slider.setColour(juce::Slider::thumbColourId, hasFocusedStep
            ? juce::Colours::white.withAlpha(0.96f)
            : juce::Colours::white.withAlpha(0.34f));
        slider.setValue(hasFocusedStep ? value : 0.0f, juce::dontSendNotification);
    };
    tintTransitionSlider(sceneTransitionMixSlider, focusedTransitionIntensity);
    tintTransitionSlider(sceneTransitionDelaySlider, focusedTransitionDelay);
    tintTransitionSlider(sceneTransitionFilterSlider, focusedTransitionFilter);
    tintTransitionSlider(sceneTransitionChopSlider, focusedTransitionChop);
    sceneTransitionSubtractButton.setToggleState(focusedTransitionSubtractsFromSceneLength, juce::dontSendNotification);
    sceneTransitionSubtractButton.setButtonText(
        sceneTransitionSubtractButtonLabel(focusedTransitionSubtractsFromSceneLength));
    rebuildSceneTransitionEndSampleChoices();
    if (!sceneTransitionEndSampleBox.isPopupActive())
    {
        juce::StringArray endSampleLabels;
        endSampleLabels.add("None");

        int selectedEndSampleId = 1;
        for (int choiceIndex = 0; choiceIndex < static_cast<int>(sceneTransitionEndSampleChoices.size()); ++choiceIndex)
        {
            const auto& file = sceneTransitionEndSampleChoices[static_cast<size_t>(choiceIndex)];
            const juce::String label = sceneTransitionEndSampleChoiceLabel(file, endSampleDirectory);
            endSampleLabels.add(label);
            if (sceneTransitionEndSampleFilesMatch(focusedTransitionEndSample, file))
                selectedEndSampleId = choiceIndex + 2;
        }

        if (selectedEndSampleId == 1
            && focusedTransitionEndSample != juce::File()
            && focusedTransitionEndSample.getFullPathName().trim().isNotEmpty())
        {
            selectedEndSampleId = endSampleLabels.size() + 1;
            endSampleLabels.add("(Missing) " + focusedTransitionEndSample.getFileName());
        }

        bool needsRebuild = sceneTransitionEndSampleBox.getNumItems() != endSampleLabels.size();
        if (!needsRebuild)
        {
            for (int itemIndex = 0; itemIndex < endSampleLabels.size(); ++itemIndex)
            {
                if (sceneTransitionEndSampleBox.getItemText(itemIndex) != endSampleLabels[itemIndex])
                {
                    needsRebuild = true;
                    break;
                }
            }
        }

        sceneTransitionEndSampleBoxUpdating = true;
        if (needsRebuild)
        {
            sceneTransitionEndSampleBox.clear(juce::dontSendNotification);
            for (int itemIndex = 0; itemIndex < endSampleLabels.size(); ++itemIndex)
                sceneTransitionEndSampleBox.addItem(endSampleLabels[itemIndex], itemIndex + 1);
        }

        const int desiredSelectedId = hasFocusedStep ? selectedEndSampleId : 0;
        if (sceneTransitionEndSampleBox.getSelectedId() != desiredSelectedId)
            sceneTransitionEndSampleBox.setSelectedId(desiredSelectedId, juce::dontSendNotification);
        sceneTransitionEndSampleBoxUpdating = false;
    }
    sceneTransitionSummaryLabel.setVisible(true);
    sceneTransitionMetaLabel.setVisible(true);
    sceneTransitionSmartLinkButton.setEnabled(hasFocusedStep);
    sceneTransitionSmartLinkButton.setToggleState(sceneTransitionSmartLinkEnabled, juce::dontSendNotification);
    sceneTransitionSmartLinkButton.setButtonText(sceneTransitionSmartLinkEnabled ? "Linked" : "Manual");
    sceneTransitionHeaderLabel.setColour(juce::Label::textColourId,
                                         hasFocusedStep ? transitionColour.brighter(0.20f) : kTextMuted);
    sceneTransitionOptionsHeaderLabel.setColour(juce::Label::textColourId,
                                                hasFocusedStep ? transitionColour.withAlpha(0.90f) : kTextMuted);
    sceneTransitionLengthHeaderLabel.setColour(juce::Label::textColourId,
                                               hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionMixHeaderLabel.setColour(juce::Label::textColourId,
                                            hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionDelayHeaderLabel.setColour(juce::Label::textColourId,
                                              hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionFilterHeaderLabel.setColour(juce::Label::textColourId,
                                               hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionChopHeaderLabel.setColour(juce::Label::textColourId,
                                             hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionScopeHeaderLabel.setColour(juce::Label::textColourId,
                                              hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionContourHeaderLabel.setColour(juce::Label::textColourId,
                                                hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionConditionHeaderLabel.setColour(juce::Label::textColourId,
                                                  hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionEndSampleHeaderLabel.setColour(juce::Label::textColourId,
                                                  hasFocusedStep ? transitionColour.withAlpha(0.88f) : kTextMuted);
    sceneTransitionSummaryLabel.setColour(juce::Label::backgroundColourId, transitionColour.withAlpha(hasFocusedStep ? 0.18f : 0.10f));
    sceneTransitionSummaryLabel.setColour(juce::Label::outlineColourId, transitionColour.withAlpha(hasFocusedStep ? 0.40f : 0.18f));
    sceneTransitionSummaryLabel.setColour(juce::Label::textColourId, hasFocusedStep ? kTextPrimary : kTextMuted);
    sceneTransitionMetaLabel.setColour(juce::Label::backgroundColourId, transitionColour.withAlpha(hasFocusedStep ? 0.12f : 0.08f));
    sceneTransitionMetaLabel.setColour(juce::Label::outlineColourId, transitionColour.withAlpha(hasFocusedStep ? 0.24f : 0.14f));
    sceneTransitionMetaLabel.setColour(juce::Label::textColourId, hasFocusedStep ? kTextSecondary : kTextMuted);
    sceneTransitionEndSampleFolderButton.setButtonText(sceneEndSampleEditorVisible && hasFocusedStep ? "Close" : "Edit");
    sceneTransitionEndSampleFolderButton.setColour(juce::TextButton::buttonColourId,
                                                   hasFocusedStep
                                                       ? transitionColour.withAlpha(sceneEndSampleEditorVisible ? 0.92f
                                                                                                : (endSampleDirectory.isDirectory() ? 0.82f : 0.36f))
                                                       : juce::Colour(0xff3a4046));
    sceneTransitionEndSampleFolderButton.setColour(juce::TextButton::buttonOnColourId,
                                                   transitionColour.brighter(0.12f));
    sceneTransitionEndSampleFolderButton.setColour(juce::TextButton::textColourOffId,
                                                   hasFocusedStep && (sceneEndSampleEditorVisible || endSampleDirectory.isDirectory())
                                                       ? juce::Colour(0xff111111)
                                                       : kTextPrimary);
    sceneTransitionEndSampleFolderButton.setColour(juce::TextButton::textColourOnId,
                                                   juce::Colour(0xff111111));
    sceneTransitionSmartLinkButton.setColour(juce::TextButton::buttonColourId,
                                             hasFocusedStep
                                                 ? (sceneTransitionSmartLinkEnabled
                                                        ? transitionColour.withAlpha(0.88f)
                                                        : juce::Colour(0xff454b52))
                                                 : juce::Colour(0xff3a4046));
    sceneTransitionSmartLinkButton.setColour(juce::TextButton::buttonOnColourId,
                                             transitionColour.brighter(0.18f));
    sceneTransitionSmartLinkButton.setColour(juce::TextButton::textColourOffId,
                                             sceneTransitionSmartLinkEnabled && hasFocusedStep
                                                 ? juce::Colour(0xff111111)
                                                 : kTextPrimary);
    sceneTransitionSmartLinkButton.setColour(juce::TextButton::textColourOnId,
                                             juce::Colour(0xff111111));

    auto tintQuickButton = [&](juce::TextButton& button,
                               bool active,
                               juce::Colour colour)
    {
        button.setEnabled(hasFocusedStep);
        button.setColour(juce::TextButton::buttonColourId,
                         active ? colour.withAlpha(0.90f)
                                : juce::Colour(0xff3d4349));
        button.setColour(juce::TextButton::buttonOnColourId,
                         colour.brighter(0.12f));
        button.setColour(juce::TextButton::textColourOffId,
                         active ? juce::Colour(0xff111111) : kTextPrimary);
        button.setColour(juce::TextButton::textColourOnId,
                         juce::Colour(0xff111111));
    };
    tintQuickButton(sceneTransitionPunchButton,
                    hasFocusedStep
                        && focusedTransitionType == MlrVSTAudioProcessor::SceneChainTransitionType::Stutter
                        && focusedTransitionOption == MlrVSTAudioProcessor::SceneChainTransitionOption::Tight,
                    juce::Colour(0xffdf7b45));
    tintQuickButton(sceneTransitionLiftButton,
                    hasFocusedStep
                        && focusedTransitionType == MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise
                        && focusedTransitionOption == MlrVSTAudioProcessor::SceneChainTransitionOption::Sweep,
                    juce::Colour(0xff76b98f));
    tintQuickButton(sceneTransitionEchoButton,
                    hasFocusedStep
                        && focusedTransitionType == MlrVSTAudioProcessor::SceneChainTransitionType::Fill
                        && focusedTransitionOption == MlrVSTAudioProcessor::SceneChainTransitionOption::Echo,
                    juce::Colour(0xff76a9d8));
    tintQuickButton(sceneTransitionDropButton,
                    hasFocusedStep
                        && focusedTransitionType == MlrVSTAudioProcessor::SceneChainTransitionType::Drop
                        && focusedTransitionOption == MlrVSTAudioProcessor::SceneChainTransitionOption::Gate,
                    juce::Colour(0xffd5695a));

    if (hasFocusedStep)
    {
        const juce::String focusedEndSampleLabel = focusedTransitionEndSample == juce::File()
            ? juce::String("None")
            : (!focusedTransitionEndSample.existsAsFile()
                   ? juce::String("(Missing) ") + focusedTransitionEndSample.getFileName()
                   : focusedTransitionEndSample.getFileName());
        sceneTransitionSummaryLabel.setText(sceneChainTransitionTypeLabel(focusedTransitionType)
                                                + " / "
                                                + sceneChainTransitionOptionLabel(focusedTransitionOption)
                                                + " • "
                                                + sceneTransitionFocusSummary(focusedTransitionDelay,
                                                                             focusedTransitionFilter,
                                                                             focusedTransitionChop)
                                                + " • "
                                                + sceneChainTransitionScopeLabel(focusedTransitionScope),
                                            juce::dontSendNotification);
    sceneTransitionMetaLabel.setText(formatSceneTransitionLengthBeats(focusedTransitionLength)
                                             + " • "
                                             + sceneTransitionEnergySummary(focusedTransitionIntensity)
                                             + " • "
                                             + sceneChainTransitionContourLabel(focusedTransitionContour)
                                             + " • "
                                             + sceneChainTransitionConditionLabel(focusedTransitionCondition)
                                             + " • "
                                             + juce::String("D")
                                             + juce::String(juce::roundToInt(focusedTransitionDelay * 100.0f))
                                             + " F"
                                             + juce::String(juce::roundToInt(focusedTransitionFilter * 100.0f))
                                             + " G"
                                             + juce::String(juce::roundToInt(focusedTransitionChop * 100.0f))
                                             + " • "
                                             + juce::String(focusedTransitionSubtractsFromSceneLength ? "Shorten" : "Ride")
                                             + " • Picked "
                                             + focusedEndSampleLabel,
                                         juce::dontSendNotification);
        const int currentSceneSlot = processor.getSceneChainStepSceneSlot(focusedStep);
        const bool loopbackStep = processor.isSceneChainLoopEnabled() && focusedStep == (chainLength - 1);
        const int nextStep = (focusedStep + 1 < chainLength)
            ? (focusedStep + 1)
            : (processor.isSceneChainLoopEnabled() ? processor.getSceneChainLoopStartStep() : -1);
        juce::String transitionTooltip = loopbackStep
            ? (juce::String("Focused loopback clamp • step ") + juce::String(focusedStep + 1))
            : (juce::String("Focused connector • step ") + juce::String(focusedStep + 1));
        if (currentSceneSlot >= 0)
            transitionTooltip << " • S" << juce::String(currentSceneSlot + 1);
        if (nextStep >= 0 && nextStep < chainLength)
            transitionTooltip << " -> S" << juce::String(processor.getSceneChainStepSceneSlot(nextStep) + 1);
        else
            transitionTooltip << " -> stop";
        transitionTooltip << " • " << sceneChainTransitionTypeSummary(focusedTransitionType)
                          << " • " << sceneChainTransitionOptionSummary(focusedTransitionOption)
                          << " • Scope: " << sceneChainTransitionScopeSummary(focusedTransitionScope)
                          << " • Contour: " << sceneChainTransitionContourSummary(focusedTransitionContour)
                          << " • Condition: " << sceneChainTransitionConditionSummary(focusedTransitionCondition)
                          << " • " << sceneTransitionParameterSummary(focusedTransitionLength,
                                                                      focusedTransitionScope,
                                                                      focusedTransitionContour,
                                                                      focusedTransitionCondition,
                                                                      focusedTransitionIntensity,
                                                                      focusedTransitionDelay,
                                                                      focusedTransitionFilter,
                                                                      focusedTransitionChop)
                          << " • End sample: " << focusedEndSampleLabel
                          << " • Type, Style, Lead, and Blend retune the fill shape together"
                          << " • Drag X: Lead | Drag Y: Blend | Wheel: Style | Shift-wheel: Lead | Cmd/Ctrl-wheel: Blend | Alt-wheel: Type | Shift+Cmd/Ctrl-wheel: Scope | Shift+Alt-wheel: Contour | Cmd/Ctrl+Alt-wheel: When | Double-click: timing mode"
                          << " • " << sceneTransitionTimingModeSummary(focusedTransitionSubtractsFromSceneLength);
        sceneTransitionSummaryLabel.setTooltip(transitionTooltip);
        sceneTransitionMetaLabel.setTooltip(transitionTooltip);
        sceneTransitionTypeBox.setTooltip("Fill type for the focused connector bubble or loopback clamp. Changing Type also retunes the Echo, Tone, and Gate balance.");
        sceneTransitionOptionsBox.setTooltip("Transition style for the focused fill bubble. Style steers the same Chain Fill toward tighter, wider, echo-led, sweep-led, or gate-led behaviour.");
        sceneTransitionScopeBox.setTooltip("Scope decides which strip types the focused Chain Fill will touch: all strips, loop-style strips, steps, grains, or flip/sample strips.");
        sceneTransitionContourBox.setTooltip("Contour shapes how the fill evolves across its lead time: smooth, ramped, burst-ending, duck-then-lift, or late-hit.");
        sceneTransitionConditionBox.setTooltip("Condition decides when this Chain Fill is allowed to fire: every time, probabilistically, loop-only, or forward-only.");
        sceneTransitionLengthBox.setTooltip("Lead-in length for the focused fill bubble. Longer leads lean more into Echo and Tone; shorter leads stay punchier.");
        sceneTransitionMixSlider.setTooltip("Primary Chain Fill macro. Blend retunes Echo, Tone, and Gate together around the current Type, Style, and Lead.");
        sceneTransitionDelaySlider.setTooltip("Delay amount used by the focused fill bubble.");
        sceneTransitionFilterSlider.setTooltip("Filter sweep amount used by the focused fill bubble.");
        sceneTransitionChopSlider.setTooltip("Gate/stutter amount used by the focused fill bubble.");
        sceneTransitionEndSampleBox.setTooltip("Quick pick for the sample that fires on the next bar. Open Edit for audition and shaping.");
        sceneTransitionEndSampleFolderButton.setTooltip(sceneEndSampleEditorVisible
                                                            ? "Close the fill-end sample editor."
                                                            : "Open the fill-end sample editor for audition, browsing, and playback shaping.");
        sceneTransitionSubtractButton.setTooltip(focusedTransitionSubtractsFromSceneLength
                                                     ? "On: the fill lead time is subtracted from the outgoing scene duration."
                                                     : "Off: the fill rides over the scene end without shortening the scene duration.");
        sceneTransitionSmartLinkButton.setTooltip(sceneTransitionSmartLinkEnabled
                                                      ? "Linked is on. Blend, Type, Style, and Lead all keep the Chain Fill musically balanced. Touch Echo, Tone, or Gate to break out into manual editing."
                                                      : "Manual editing is on. Turn Linked back on to let Blend, Type, Style, and Lead retune the focused Chain Fill.");
        sceneTransitionPunchButton.setTooltip("Punch preset: tight stutter with a short lead and stronger gate.");
        sceneTransitionLiftButton.setTooltip("Lift preset: longer rising sweep into the next scene.");
        sceneTransitionEchoButton.setTooltip("Echo preset: delay-led Chain Fill with a softer handoff.");
        sceneTransitionDropButton.setTooltip("Drop preset: punchier cut into the next scene.");
    }
    else
    {
        sceneTransitionSummaryLabel.setText("No fill bubble", juce::dontSendNotification);
        sceneTransitionMetaLabel.setText("Click a connector chip or the loop clamp to edit the current Chain Fill",
                                         juce::dontSendNotification);
        sceneTransitionSummaryLabel.setTooltip("The fill editor follows the focused connector bubble or loopback clamp.");
        sceneTransitionMetaLabel.setTooltip("Select a connector chip or loop clamp, then shape the fill here.");
        sceneTransitionTypeBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionOptionsBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionScopeBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionContourBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionConditionBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionLengthBox.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionMixSlider.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionDelaySlider.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionFilterSlider.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionChopSlider.setTooltip("Select a connector chip or loop clamp to edit fills.");
        sceneTransitionEndSampleBox.setTooltip("Select a connector chip or loop clamp first, then choose its next-bar end sample.");
        sceneTransitionEndSampleFolderButton.setTooltip("Select a connector chip or loop clamp first, then open the fill-end sample editor.");
        sceneTransitionSubtractButton.setTooltip("Select a connector chip or loop clamp, then decide whether its fill shortens the outgoing scene.");
        sceneTransitionSmartLinkButton.setTooltip("Select a connector chip or loop clamp, then use Linked mode to keep that Chain Fill musically balanced while you edit it.");
        sceneTransitionPunchButton.setTooltip("Select a connector chip or loop clamp first.");
        sceneTransitionLiftButton.setTooltip("Select a connector chip or loop clamp first.");
        sceneTransitionEchoButton.setTooltip("Select a connector chip or loop clamp first.");
        sceneTransitionDropButton.setTooltip("Select a connector chip or loop clamp first.");
    }

    const auto sceneWatcher = processor.getSceneWatcherState();
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        const auto idx = static_cast<size_t>(sceneSlot);
        const auto& runtimeSlot = sceneWatcher.slots[idx];
        const auto& clipSlot = runtimeSlot.clipSlot;
        const auto lengthMode = processor.getSceneLengthMode(sceneSlot);
        const int countValue = processor.getSceneLengthCount(sceneSlot);
        const int anchorStrip = processor.getSceneAnchorStrip(sceneSlot);
        const double resolvedBeats = processor.getResolvedSceneLengthBeats(sceneSlot);
        const double totalBeats = processor.getSceneAdvanceLengthBeats(sceneSlot);
        const int sequenceStepIndex = clipSlot.chainStepIndex;
        const bool chainPlaybackActive = sceneWatcher.chainActive;
        const bool isActiveScene = runtimeSlot.active;
        const bool isQueuedScene = runtimeSlot.queued;
        const bool isQueuedManualScene = isQueuedScene && sceneWatcher.manualPlaybackOwned;
        const bool isSelectedScene = runtimeSlot.focused;
        const bool hasStoredSceneData = clipSlot.hasStoredContent;
        juce::ignoreUnused(resolvedBeats);
        juce::String resolvedTooltip = hasStoredSceneData
            ? juce::String("Stored scene data")
            : juce::String("Empty scene slot | no saved scene data yet");
        resolvedTooltip << " | " << sceneLengthModeSummary(processor, sceneSlot)
                        << " | " << juce::String(totalBeats, 2) << " beats";
        if (sequenceStepIndex >= 0)
            resolvedTooltip << " | Chain step " << juce::String(sequenceStepIndex + 1);
        if (isActiveScene)
            resolvedTooltip << " | Active";
        if (isQueuedScene)
            resolvedTooltip << " | Next";
        if (isSelectedScene)
            resolvedTooltip << " | Focused in editor";

        sceneLengthModeBoxes[idx].setSelectedId(static_cast<int>(lengthMode) + 1, juce::dontSendNotification);
        sceneManualBarsBoxes[idx].setSelectedId(countValue, juce::dontSendNotification);
        sceneAnchorStripBoxes[idx].setSelectedId(anchorStrip + 1, juce::dontSendNotification);
        sceneLengthModeBoxes[idx].setVisible(isSelectedScene);
        sceneManualBarsBoxes[idx].setVisible(isSelectedScene);
        sceneAnchorStripBoxes[idx].setVisible(isSelectedScene && lengthMode == MlrVSTAudioProcessor::SceneLengthMode::AnchorStrip);
        sceneAnchorStripBoxes[idx].setEnabled(lengthMode == MlrVSTAudioProcessor::SceneLengthMode::AnchorStrip);
        juce::String slotText = "S" + juce::String(sceneSlot + 1);
        if (isActiveScene)
            slotText << " LIVE";
        else if (isQueuedScene)
            slotText << " NEXT";
        else if (chainPlaybackActive && sequenceStepIndex >= 0)
            slotText << " #" << juce::String(sequenceStepIndex + 1);
        auto& selectorButton = sceneSelectorButtons[idx];
        selectorButton.setButtonText(slotText);
        selectorButton.setToggleState(isSelectedScene, juce::dontSendNotification);
        selectorButton.getProperties().set("sceneHasStoredData", hasStoredSceneData);
        selectorButton.getProperties().set("sceneDataBadgeText",
                                           hasStoredSceneData ? juce::String()
                                                              : juce::String("EMPTY"));
        const auto identityColour = sceneSlotUiColour(processor, sceneSlot);
        const auto emptySlotColour = juce::Colour(0xff394047);
        const auto emptySlotFocusColour = juce::Colour(0xff56606b);
        const auto queuedBlinkColour = hasStoredSceneData
            ? identityColour.interpolatedWith(juce::Colour(0xffd6a345), 0.36f)
            : emptySlotFocusColour.interpolatedWith(juce::Colour(0xffd6a345), 0.18f);
        juce::Colour slotColour = hasStoredSceneData
            ? identityColour.withAlpha(0.40f)
            : emptySlotColour.withAlpha(0.92f);
        if (isActiveScene)
            slotColour = hasStoredSceneData
                ? identityColour.withMultipliedBrightness(1.08f)
                : emptySlotFocusColour;
        else if (isQueuedManualScene)
            slotColour = queuedSceneBlinkOn
                ? queuedBlinkColour
                : (hasStoredSceneData ? identityColour.withAlpha(0.28f)
                                      : emptySlotColour.withAlpha(0.82f));
        else if (isQueuedScene)
            slotColour = queuedBlinkColour;
        else if (isSelectedScene)
            slotColour = hasStoredSceneData
                ? identityColour.withAlpha(0.58f)
                : emptySlotFocusColour.withAlpha(0.94f);
        else if (chainPlaybackActive && sequenceStepIndex >= 0)
            slotColour = hasStoredSceneData
                ? identityColour.withAlpha(0.72f)
                : emptySlotFocusColour.withAlpha(0.88f);
        selectorButton.setColour(juce::TextButton::buttonColourId, slotColour);
        selectorButton.setColour(juce::TextButton::buttonOnColourId, slotColour.brighter(0.16f));
        selectorButton.setColour(juce::TextButton::textColourOffId,
                                 (isActiveScene || isQueuedScene || (chainPlaybackActive && sequenceStepIndex >= 0))
                                     ? juce::Colour(0xff101214)
                                     : (hasStoredSceneData ? kTextPrimary
                                                           : kTextMuted.withAlpha(0.90f)));
        selectorButton.setColour(juce::TextButton::textColourOnId,
                                 (isActiveScene || isQueuedScene || (chainPlaybackActive && sequenceStepIndex >= 0))
                                     ? juce::Colour(0xff0f0f0f)
                                     : (hasStoredSceneData ? juce::Colour(0xfffafafa)
                                                           : juce::Colour(0xffdde4e9)));
        selectorButton.setTooltip(resolvedTooltip
                                  + (hasStoredSceneData
                                         ? " | This scene has saved content."
                                         : " | Empty scenes stay grey until you save into them.")
                                  + " | Click manually launches this scene on the global trigger grid and takes over from chain playback."
                                  + " | This button also focuses the scene for Copy, Paste, Clear, Length, and Count editing."
                                  + " | Drag this scene into the chain rail below to add it to playback order."
                                  + " | The chain rail below also supports drag-reorder."
                                  + " | Use the scene Copy/Paste buttons above for duplication.");
        sceneLengthModeBoxes[idx].setTooltip(resolvedTooltip + " | Length mode");
        sceneManualBarsBoxes[idx].setTooltip(resolvedTooltip + " | Length count");
        sceneAnchorStripBoxes[idx].setTooltip(resolvedTooltip + " | Anchor strip");
    }

    const bool hasEngine = (processor.getAudioEngine() != nullptr);
    auto& mutableProcessor = processor;
    for (int stripIndex = 0; stripIndex < getVisibleSceneStripCount(); ++stripIndex)
    {
        for (int lane = 0; lane < SceneAutomationLaneCount; ++lane)
        {
            auto& targetBox = sceneMotionTargetBoxes[static_cast<size_t>(stripIndex)][static_cast<size_t>(lane)];
            const auto selectedTarget = hasEngine
                ? sceneDisplayedModTargetForLane(mutableProcessor, stripIndex, lane)
                : ModernAudioEngine::ModTarget::None;
            targetBox.setEnabled(hasEngine && sceneLaneCanShowMotionTargetSelector(mutableProcessor, stripIndex, lane));
            targetBox.setSelectedId(performanceTargetToComboId(selectedTarget),
                                    juce::dontSendNotification);
        }
    }

    if (sceneLegacyModEditorVisible && sceneLegacyModEditor != nullptr)
    {
        if (sceneLegacyModStripIndex >= 0 && sceneLegacyModSlotIndex >= 0)
        {
            const bool sceneStepMotionMonomePageActive = processor.isSceneModeEnabled()
                && processor.getSceneModPageMode() == MlrVSTAudioProcessor::SceneModPageMode::StepMotion;
            if (sceneStepMotionMonomePageActive)
            {
                if (auto* engine = processor.getAudioEngine())
                {
                    sceneLegacyModStripIndex = juce::jlimit(0,
                                                            juce::jmax(0, getVisibleSceneStripCount() - 1),
                                                            processor.getLastMonomePressedStripRow());
                    sceneLegacyModSlotIndex = juce::jlimit(0,
                                                           ModernAudioEngine::NumModSequencers - 1,
                                                           engine->getModSequencerSlot(sceneLegacyModStripIndex));
                    sceneEditorState.selectedStripIndex = sceneLegacyModStripIndex;
                }
            }

            sceneLegacyModEditor->setPinnedStripAndSlot(
                sceneLegacyModStripIndex,
                sceneLegacyModSlotIndex,
                "Scene Motion  Strip " + juce::String(sceneLegacyModStripIndex + 1) + "  M" + juce::String(sceneLegacyModSlotIndex + 1));
        }
        else
        {
            sceneLegacyModEditor->clearPinnedStripAndSlot();
        }
    }

    const double beat = (processor.getAudioEngine() != nullptr)
        ? processor.getAudioEngine()->getCurrentBeat()
        : 0.0;
    updateSceneEditorState(beat);
    updateSceneTimelineContentSize();
    updateSceneViewportFollow();
    sceneChainCanvas.repaint();
    sceneTimelineCanvas.repaint();
}

void SceneControlPanel::paintSceneChainCanvas(juce::Graphics& g) const
{
    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    g.setColour(juce::Colour(0xff161b1f));
    g.fillRoundedRectangle(layout.bounds, 8.0f);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawRoundedRectangle(layout.bounds.reduced(0.5f), 8.0f, 1.0f);

    const int chainLength = processor.getSceneChainLength();
    const bool loopEnabled = processor.isSceneChainLoopEnabled();
    const int loopStart = loopEnabled ? processor.getSceneChainLoopStartStep() : -1;
    const int loopEnd = loopEnabled ? processor.getSceneChainLoopEndStep() : -1;
    const int activeStep = processor.isSceneChainPlaybackActive() ? processor.getSceneChainPlaybackStepIndex() : -1;
    const int focusedStep = getFocusedSceneChainStep();
    const int externalDropHighlightStep = (sceneSlotDragSource >= 0
                                           && sceneSlotDragMoved
                                           && sceneChainExternalDropStep >= 0)
        ? juce::jlimit(0,
                       MlrVSTAudioProcessor::MaxSceneChainSteps - 1,
                       sceneChainExternalDropStep)
        : -1;
    const int queuedSceneSlot = processor.getQueuedSceneSlot();
    int queuedStep = -1;
    for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
    {
        if (processor.getSceneChainStepSceneSlot(stepIndex) == queuedSceneSlot)
        {
            queuedStep = stepIndex;
            break;
        }
    }

    if (chainLength > 0)
    {
        const float railCenterY = layout.stepBounds.front().getCentreY();
        g.setColour(juce::Colours::white.withAlpha(0.045f));
        g.drawLine(layout.railBounds.getX() + 4.0f,
                   railCenterY,
                   layout.railBounds.getRight() - 4.0f,
                   railCenterY,
                   1.0f);
    }

    if (chainLength > 1)
    {
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        for (int stepIndex = 0; stepIndex < chainLength - 1; ++stepIndex)
        {
            const auto from = layout.stepBounds[static_cast<size_t>(stepIndex)];
            const auto to = layout.stepBounds[static_cast<size_t>(stepIndex + 1)];
            const auto chipBounds = layout.transitionBounds[static_cast<size_t>(stepIndex)];
            const float centerY = chipBounds.getCentreY();
            const auto transitionType = processor.getSceneChainStepTransitionType(stepIndex);
            const bool hovered = stepIndex == sceneChainHoverTransition;
            const bool focused = stepIndex == focusedStep;
            const auto chipColour = sceneChainTransitionColour(transitionType);
            const auto chipSecondaryColour = sceneChainTransitionSecondaryColour(transitionType);
            const bool noTransition = transitionType == MlrVSTAudioProcessor::SceneChainTransitionType::None;
            const bool transitionLive = !noTransition
                && processor.getActiveSceneBoundaryTransitionStep() == stepIndex;
            const auto connectorColour = noTransition
                ? juce::Colours::white.withAlpha(0.12f)
                : chipColour.withAlpha((hovered || focused || transitionLive) ? 0.34f : 0.22f);
            g.setColour(connectorColour);
            g.drawLine(from.getRight() + 2.0f,
                       centerY,
                       chipBounds.getX() - 2.0f,
                       centerY,
                       noTransition ? 1.1f : 1.35f);
            g.drawLine(chipBounds.getRight() + 2.0f,
                       centerY,
                       to.getX() - 2.0f,
                       centerY,
                       noTransition ? 1.1f : 1.35f);

            const auto chipOutline = chipColour.withAlpha(noTransition
                                                              ? ((hovered || focused) ? 0.76f : 0.54f)
                                                              : ((hovered || focused) ? 0.86f : 0.62f));
            if (focused)
            {
                g.setColour(chipColour.withAlpha(noTransition ? 0.08f : 0.14f));
                g.fillRoundedRectangle(chipBounds.expanded(4.0f, 3.0f), 8.0f);
            }
            if (transitionLive)
            {
                const float pulse = 0.5f + (0.5f * std::sin(static_cast<float>(
                    juce::Time::getMillisecondCounterHiRes() * 0.0105)));
                g.setColour(chipColour.withAlpha(0.10f + (0.10f * pulse)));
                g.fillRoundedRectangle(chipBounds.expanded(3.0f, 2.5f), 7.5f);
            }
            g.setColour(juce::Colours::black.withAlpha(0.22f));
            g.fillRoundedRectangle(chipBounds.translated(0.0f, 1.0f), 5.8f);
            juce::ColourGradient chipGradient(chipSecondaryColour.withAlpha(noTransition
                                                                                ? ((hovered || focused) ? 0.58f : 0.44f)
                                                                                : ((hovered || focused || transitionLive) ? 0.94f : 0.82f)),
                                              chipBounds.getX(),
                                              chipBounds.getBottom(),
                                              chipColour.withAlpha(noTransition
                                                                       ? ((hovered || focused) ? 0.32f : 0.20f)
                                                                       : ((hovered || focused || transitionLive) ? 0.98f : 0.88f)),
                                              chipBounds.getRight(),
                                              chipBounds.getY(),
                                              false);
            g.setGradientFill(chipGradient);
            g.fillRoundedRectangle(chipBounds, 5.8f);
            if (!noTransition)
            {
                auto topSheen = chipBounds.reduced(1.0f).removeFromTop(chipBounds.getHeight() * 0.45f);
                juce::ColourGradient sheenGradient(juce::Colours::white.withAlpha((hovered || focused) ? 0.14f : 0.09f),
                                                   topSheen.getCentreX(),
                                                   topSheen.getY(),
                                                   juce::Colours::transparentBlack,
                                                   topSheen.getCentreX(),
                                                   topSheen.getBottom(),
                                                   false);
                g.setGradientFill(sheenGradient);
                g.fillRoundedRectangle(topSheen, 4.8f);
            }
            g.setColour(chipOutline);
            g.drawRoundedRectangle(chipBounds.reduced(0.5f), 5.8f, (hovered || focused) ? 1.15f : 1.0f);

            if (transitionLive)
            {
                const float pulse = 0.5f + (0.5f * std::sin(static_cast<float>(
                    juce::Time::getMillisecondCounterHiRes() * 0.0105)));
                g.setColour(juce::Colours::white.withAlpha(0.62f + (0.22f * pulse)));
                g.drawRoundedRectangle(chipBounds.expanded(1.4f, 1.2f), 6.1f, 1.45f);
                g.setColour(chipColour.brighter(0.45f).withAlpha(0.52f + (0.20f * pulse)));
                g.drawRoundedRectangle(chipBounds.expanded(2.4f, 2.0f), 7.2f, 1.0f);
            }

            auto chipLabelBounds = chipBounds.reduced(4.0f, 2.0f);
            g.setColour(noTransition
                            ? kTextPrimary.withAlpha((hovered || focused) ? 0.92f : 0.78f)
                            : chipColour.contrasting(0.82f).withAlpha((hovered || focused) ? 0.98f : 0.94f));
            g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
            g.drawFittedText(sceneChainTransitionChipLabel(transitionType),
                             chipLabelBounds.toNearestInt(),
                             juce::Justification::centred,
                             1);

            if (hovered || focused)
            {
                g.setColour((focused ? juce::Colours::white.withAlpha(0.34f)
                                     : juce::Colours::white.withAlpha(0.22f)));
                g.drawRoundedRectangle(chipBounds.reduced(0.5f), 6.0f, 1.0f);
            }

            if (sceneChainDragTransition == stepIndex)
            {
                const juce::String dragText = formatSceneTransitionLengthBeats(
                                                  processor.getSceneChainStepTransitionLengthBeats(stepIndex))
                    + " • "
                    + juce::String(juce::roundToInt(processor.getSceneChainStepTransitionIntensity(stepIndex) * 100.0f))
                    + "%";
                auto dragBounds = juce::Rectangle<float>(0.0f,
                                                         chipBounds.getBottom() + 4.0f,
                                                         juce::jlimit(42.0f,
                                                                      78.0f,
                                                                      18.0f + (static_cast<float>(dragText.length()) * 4.2f)),
                                                         10.0f)
                    .withCentre({ chipBounds.getCentreX(), chipBounds.getBottom() + 9.0f });
                dragBounds.setX(juce::jlimit(layout.bounds.getX() + 4.0f,
                                             layout.bounds.getRight() - dragBounds.getWidth() - 4.0f,
                                             dragBounds.getX()));
                g.setColour(juce::Colour(0xff0f1317).withAlpha(0.96f));
                g.fillRoundedRectangle(dragBounds, 4.0f);
                g.setColour(chipColour.withAlpha(0.82f));
                g.drawRoundedRectangle(dragBounds.reduced(0.5f), 4.0f, 0.95f);
                g.setColour(kTextPrimary.withAlpha(0.98f));
                g.setFont(juce::Font(juce::FontOptions(5.8f, juce::Font::bold)));
                g.drawFittedText(dragText,
                                 dragBounds.reduced(3.0f, 0.0f).toNearestInt(),
                                 juce::Justification::centred,
                                 1);
            }
        }
    }

    if (loopEnabled && chainLength >= 2 && loopStart >= 0 && loopEnd >= loopStart)
    {
        const auto loopLeft = layout.stepBounds[static_cast<size_t>(loopStart)].getX() + 4.0f;
        const auto loopRight = layout.stepBounds[static_cast<size_t>(loopEnd)].getRight() - 4.0f;
        const bool chainActive = processor.isSceneChainPlaybackActive();
        const auto loopColour = chainActive ? kAccent.withAlpha(0.92f)
                                            : juce::Colours::white.withAlpha(0.28f);
        const auto clampBounds = sceneChainLoopClampBounds(layout, loopStart, loopEnd);
        g.setColour(loopColour.withAlpha(chainActive ? 0.22f : 0.10f));
        g.fillRoundedRectangle(clampBounds, 5.4f);
        g.setColour(loopColour.withAlpha(chainActive ? 0.98f : 0.56f));
        g.drawRoundedRectangle(clampBounds.reduced(0.4f), 5.4f, chainActive ? 1.5f : 1.1f);

        const auto leftClamp = juce::Rectangle<float>(loopLeft - 4.0f,
                                                      clampBounds.getY() - 1.0f,
                                                      6.0f,
                                                      clampBounds.getHeight() + 2.0f);
        const auto rightClamp = juce::Rectangle<float>(loopRight - 2.0f,
                                                       clampBounds.getY() - 1.0f,
                                                       6.0f,
                                                       clampBounds.getHeight() + 2.0f);
        g.setColour(loopColour.withAlpha(chainActive ? 0.96f : 0.68f));
        g.fillRoundedRectangle(leftClamp, 2.2f);
        g.fillRoundedRectangle(rightClamp, 2.2f);

        const float iconSize = 10.0f;
        const juce::Rectangle<float> iconBounds(((loopLeft + loopRight) * 0.5f) - (iconSize * 0.5f),
                                                clampBounds.getCentreY() - (iconSize * 0.5f),
                                                iconSize,
                                                iconSize);
        g.setColour(juce::Colour(0xff101214).withAlpha(chainActive ? 0.88f : 0.68f));
        g.fillRoundedRectangle(iconBounds.expanded(4.0f, 1.0f), 5.0f);
        juce::Path loopIcon;
        loopIcon.addCentredArc(iconBounds.getCentreX(),
                               iconBounds.getCentreY(),
                               3.2f,
                               3.2f,
                               0.0f,
                               juce::MathConstants<float>::pi * 0.18f,
                               juce::MathConstants<float>::pi * 1.56f,
                               true);
        loopIcon.startNewSubPath(iconBounds.getCentreX() + 1.7f, iconBounds.getY() + 1.6f);
        loopIcon.lineTo(iconBounds.getRight() - 1.4f, iconBounds.getY() + 1.9f);
        loopIcon.lineTo(iconBounds.getRight() - 2.5f, iconBounds.getY() + 4.1f);
        loopIcon.startNewSubPath(iconBounds.getCentreX() - 1.7f, iconBounds.getBottom() - 1.6f);
        loopIcon.lineTo(iconBounds.getX() + 1.4f, iconBounds.getBottom() - 1.9f);
        loopIcon.lineTo(iconBounds.getX() + 2.5f, iconBounds.getBottom() - 4.1f);
        g.strokePath(loopIcon, juce::PathStrokeType(1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps; ++stepIndex)
    {
        auto stepBounds = layout.stepBounds[static_cast<size_t>(stepIndex)];
        const auto drawBounds = stepBounds;
        const bool occupied = stepIndex < chainLength;
        const int sceneSlot = occupied ? processor.getSceneChainStepSceneSlot(stepIndex) : -1;
        const bool active = stepIndex == activeStep;
        const bool focused = occupied && stepIndex == focusedStep;
        const bool queued = occupied && stepIndex == queuedStep && !active;
        const bool hovered = sceneSlotDragSource < 0 && stepIndex == sceneChainHoverStep;
        const bool dropTarget = sceneChainDragSourceStep >= 0
            && stepIndex == sceneChainDragTargetStep
            && sceneChainDragSourceStep != sceneChainDragTargetStep;
        const bool externalDropTarget = stepIndex == externalDropHighlightStep;
        const auto dropHighlightColour = externalDropTarget
            ? sceneSlotUiColour(processor, sceneSlotDragSource).withAlpha(0.96f)
            : kAccent.withAlpha(0.95f);
        const auto slotColour = occupied ? sceneSlotUiColour(processor, sceneSlot) : juce::Colour(0xff2a3035);
        const auto fill = occupied
            ? (active ? slotColour.withAlpha(0.86f)
                      : (queued ? slotColour.withAlpha(0.58f)
                                : (focused ? slotColour.withAlpha(0.38f)
                                           : slotColour.withAlpha(0.28f))))
            : (externalDropTarget
                   ? sceneSlotUiColour(processor, sceneSlotDragSource).withAlpha(0.18f)
                   : juce::Colour(0xff1f2529));
        const auto outline = active ? juce::Colours::white.withAlpha(0.92f)
            : (queued ? juce::Colour(0xfff0c567)
                      : (focused ? slotColour.withAlpha(0.98f)
                      : ((dropTarget || externalDropTarget)
                             ? dropHighlightColour
                                     : (hovered ? juce::Colours::white.withAlpha(0.3f)
                                                : juce::Colours::white.withAlpha(0.1f)))));
        if (focused && !active && !queued)
        {
            g.setColour(slotColour.withAlpha(0.12f));
            g.fillRoundedRectangle(drawBounds.expanded(2.5f, 2.5f), 8.5f);
        }
        g.setColour(fill);
        g.fillRoundedRectangle(drawBounds, 7.0f);
        g.setColour(outline);
        g.drawRoundedRectangle(drawBounds.reduced(0.5f), 7.0f, (active || focused) ? 1.4f : 1.0f);

        if (dropTarget || externalDropTarget)
        {
            g.setColour((externalDropTarget
                             ? sceneSlotUiColour(processor, sceneSlotDragSource)
                             : kAccent)
                            .withAlpha(0.22f));
            g.fillRoundedRectangle(drawBounds.reduced(2.5f, 2.5f), 5.0f);
        }

        auto bodyBounds = stepBounds.reduced(4.5f, 4.5f);
        if (occupied)
        {
            const auto sceneName = sceneSlotShortDisplayName(processor, sceneSlot, 18);
            const juce::String sceneNameText = sceneName.isNotEmpty()
                ? sceneName
                : ("Scene " + juce::String(sceneSlot + 1));
            auto topRow = bodyBounds.removeFromTop(11.0f);
            auto nameBounds = bodyBounds;

            auto slotBadge = topRow.removeFromLeft(18.0f);
            g.setColour(juce::Colour(0xff0f1215).withAlpha(active ? 0.88f : 0.74f));
            g.fillRoundedRectangle(slotBadge, 3.5f);
            g.setColour(active ? juce::Colours::white.withAlpha(0.94f) : kTextPrimary.withAlpha(0.88f));
            g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
            g.drawText("S" + juce::String(sceneSlot + 1),
                       slotBadge.toNearestInt(),
                       juce::Justification::centred,
                       false);

            g.setColour(juce::Colour(0xff0f1215).withAlpha(0.72f));
            g.setFont(juce::Font(juce::FontOptions(6.8f, juce::Font::bold)));
            g.drawText("#" + juce::String(stepIndex + 1),
                       juce::Rectangle<float>(topRow.getX() + 2.0f, topRow.getY(), 18.0f, topRow.getHeight()).toNearestInt(),
                       juce::Justification::centredLeft,
                       false);

            if (active || queued)
            {
                g.setColour(active ? juce::Colour(0xff0f1215).withAlpha(0.92f) : juce::Colour(0xfff6e2aa).withAlpha(0.92f));
                g.setFont(juce::Font(juce::FontOptions(6.8f, juce::Font::bold)));
                g.drawText(active ? "LIVE" : "NEXT",
                           topRow.toNearestInt(),
                           juce::Justification::centredRight,
                           false);
            }

            g.setColour(active ? juce::Colour(0xff0e1114)
                               : (focused ? kTextPrimary.withAlpha(0.98f) : kTextPrimary.withAlpha(0.92f)));
            g.setFont(juce::Font(juce::FontOptions(8.8f, juce::Font::bold)));
            g.drawFittedText(sceneNameText,
                             nameBounds.toNearestInt(),
                             juce::Justification::centred,
                             2,
                             0.90f);
        }
        else
        {
            const bool nextInsertSlot = stepIndex == chainLength;
            g.setColour(kTextMuted.withAlpha(nextInsertSlot ? 0.82f : 0.36f));
            g.setFont(juce::Font(juce::FontOptions(7.8f, juce::Font::bold)));
            g.drawText(nextInsertSlot ? "Add" : "Empty",
                       bodyBounds.toNearestInt(),
                       juce::Justification::centred,
                       false);
        }
    }

    if (sceneSlotDragSource >= 0 && sceneChainExternalDropStep >= 0)
    {
        const float markerX = sceneChainInsertionMarkerX(layout, sceneChainExternalDropStep);
        const auto markerColour = sceneSlotUiColour(processor, sceneSlotDragSource).withAlpha(0.96f);
        const int ghostStepIndex = juce::jlimit(0,
                                                MlrVSTAudioProcessor::MaxSceneChainSteps - 1,
                                                sceneChainExternalDropStep);
        const auto ghostBounds = layout.stepBounds[static_cast<size_t>(ghostStepIndex)].reduced(0.5f, 1.0f);
        const auto markerTag = juce::Rectangle<float>(markerX - 22.0f,
                                                      ghostBounds.getY() - 18.0f,
                                                      44.0f,
                                                      13.0f);
        g.setColour(markerColour.withAlpha(0.16f));
        g.fillRoundedRectangle(ghostBounds, 7.0f);
        g.setColour(markerColour.withAlpha(0.42f));
        g.drawRoundedRectangle(ghostBounds.reduced(0.5f), 7.0f, 1.1f);
        g.setColour(markerColour);
        g.drawLine(markerX,
                   layout.railBounds.getY() + 3.0f,
                   markerX,
                   layout.railBounds.getBottom() - 3.0f,
                   2.4f);
        g.fillRoundedRectangle(markerTag, 5.0f);
        g.setColour(juce::Colour(0xff101214).withAlpha(0.95f));
        g.drawRoundedRectangle(markerTag.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(juce::Colour(0xff101214));
        g.setFont(juce::Font(juce::FontOptions(7.4f, juce::Font::bold)));
        g.drawText("DROP S" + juce::String(sceneSlotDragSource + 1),
                   markerTag.toNearestInt(),
                   juce::Justification::centred,
                   false);
    }

    if (chainLength == 0)
    {
        g.setColour(kTextMuted.withAlpha(0.82f));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText("No steps yet. Click the first empty cell to add the focused scene.",
                   layout.railBounds.toNearestInt(),
                   juce::Justification::centred,
                   false);
    }
}

void SceneControlPanel::handleSceneChainMouseDown(const juce::MouseEvent& e)
{
    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    const int hitStep = sceneChainStepAtPosition(layout, e.position);
    int hitTransition = sceneChainTransitionAtPosition(layout, e.position);
    sceneChainHoverStep = hitStep;
    sceneChainHoverTransition = hitTransition;
    sceneChainDragSourceStep = -1;
    sceneChainDragTargetStep = -1;
    sceneChainDragMoved = false;
    sceneChainDragTransition = -1;
    sceneChainDragTransitionMoved = false;

    auto chainSteps = snapshotSceneChainSteps(processor);
    const int chainLength = static_cast<int>(chainSteps.size());
    const bool loopEnabled = processor.isSceneChainLoopEnabled();
    const int loopStart = processor.getSceneChainLoopStartStep();
    const int loopEnd = processor.getSceneChainLoopEndStep();
    const auto loopClampBounds = sceneChainLoopClampBounds(layout, loopStart, loopEnd);

    if (loopEnabled && !loopClampBounds.isEmpty() && loopClampBounds.expanded(4.0f, 6.0f).contains(e.position))
    {
        hitTransition = chainLength - 1;
        sceneChainHoverTransition = hitTransition;
    }

    if (hitTransition >= 0 && hitTransition < chainLength)
    {
        selectedSceneChainStep = hitTransition;
        sceneChainDragTransition = hitTransition;
        sceneChainDragTransitionStartPosition = e.position;
        sceneChainDragTransitionStartLengthBeats =
            processor.getSceneChainStepTransitionLengthBeats(hitTransition);
        sceneChainDragTransitionStartIntensity =
            processor.getSceneChainStepTransitionIntensity(hitTransition);
        refreshFromProcessor();
        return;
    }

    if (hitStep < 0)
    {
        sceneChainCanvas.repaint();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (hitStep < chainLength)
        {
            chainSteps.erase(chainSteps.begin() + hitStep);
            applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
            refreshFromProcessor();
        }
        return;
    }

    if (e.mods.isAltDown())
    {
        if (hitStep < chainLength)
        {
            chainSteps.erase(chainSteps.begin() + hitStep);
            applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
            refreshFromProcessor();
        }
        return;
    }

    if (hitStep >= chainLength)
    {
        if (chainLength >= MlrVSTAudioProcessor::MaxSceneChainSteps)
            return;

        MlrVSTAudioProcessor::SceneChainStep newStep;
        newStep.sceneSlot = getFocusedSceneSlot();
        newStep.repeats = 1;
        chainSteps.insert(chainSteps.begin() + juce::jmin(hitStep, chainLength), newStep);
        applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
        selectedSceneChainStep = juce::jmin(hitStep, chainLength);
        refreshFromProcessor();
        return;
    }

        const int focusedChainSceneSlot = chainSteps[static_cast<size_t>(hitStep)].sceneSlot;
    if (focusedChainSceneSlot >= 0 && focusedChainSceneSlot < MlrVSTAudioProcessor::SceneSlots)
    {
        selectedSceneChainStep = hitStep;
        selectedSceneActionSlot = focusedChainSceneSlot;
        processor.focusSceneSlot(focusedChainSceneSlot);
        refreshFromProcessor();
    }

    sceneChainDragSourceStep = hitStep;
    sceneChainDragTargetStep = hitStep;
}

void SceneControlPanel::handleSceneChainMouseDoubleClick(const juce::MouseEvent& e)
{
    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    const int chainLength = processor.getSceneChainLength();
    if (chainLength <= 0)
        return;

    int hitTransition = sceneChainTransitionAtPosition(layout, e.position);
    if (processor.isSceneChainLoopEnabled() && chainLength >= 2)
    {
        const auto loopClampBounds = sceneChainLoopClampBounds(layout,
                                                               processor.getSceneChainLoopStartStep(),
                                                               processor.getSceneChainLoopEndStep());
        if (loopClampBounds.expanded(4.0f, 6.0f).contains(e.position))
            hitTransition = chainLength - 1;
    }

    if (hitTransition < 0 || hitTransition >= chainLength)
        return;

    const auto transitionType = processor.getSceneChainStepTransitionType(hitTransition);
    if (transitionType == MlrVSTAudioProcessor::SceneChainTransitionType::None)
        return;

    selectedSceneChainStep = hitTransition;
    processor.setSceneChainStepTransitionSubtractsFromSceneLength(
        hitTransition,
        !processor.getSceneChainStepTransitionSubtractsFromSceneLength(hitTransition));
    refreshFromProcessor();
}

void SceneControlPanel::handleSceneChainMouseDrag(const juce::MouseEvent& e)
{
    if (sceneChainDragTransition >= 0)
    {
        const int hitTransition = sceneChainDragTransition;
        const int chainLength = processor.getSceneChainLength();
        if (hitTransition < 0 || hitTransition >= chainLength)
            return;

        const float deltaX = e.position.x - sceneChainDragTransitionStartPosition.x;
        const float deltaY = sceneChainDragTransitionStartPosition.y - e.position.y;
        if (std::abs(deltaX) > 3.0f || std::abs(deltaY) > 3.0f)
            sceneChainDragTransitionMoved = true;

        if (!sceneChainDragTransitionMoved)
        {
            sceneChainCanvas.repaint();
            return;
        }

        const float newLength = sceneTransitionLengthBeatsForDrag(sceneChainDragTransitionStartLengthBeats,
                                                                  deltaX);
        const float newIntensity = juce::jlimit(0.0f,
                                                1.0f,
                                                sceneChainDragTransitionStartIntensity + (deltaY / 90.0f));
        const auto currentType = processor.getSceneChainStepTransitionType(hitTransition);
        const auto currentOption = processor.getSceneChainStepTransitionOption(hitTransition);
        const bool subtractFromSceneLength =
            processor.getSceneChainStepTransitionSubtractsFromSceneLength(hitTransition);
        const bool linkedType = currentType != MlrVSTAudioProcessor::SceneChainTransitionType::None
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::Return;
        const bool canUseLinkedRetune = sceneTransitionSmartLinkEnabled && linkedType;

        if (canUseLinkedRetune)
        {
            applySceneTransitionSmartTuning(hitTransition,
                                            currentType,
                                            currentOption,
                                            newIntensity,
                                            newLength,
                                            subtractFromSceneLength,
                                            false,
                                            true);
        }
        else
        {
            const bool lengthChanged = std::abs(processor.getSceneChainStepTransitionLengthBeats(hitTransition) - newLength)
                > 1.0e-4f;
            const bool intensityChanged = std::abs(processor.getSceneChainStepTransitionIntensity(hitTransition) - newIntensity)
                > 1.0e-4f;
            if (!lengthChanged && !intensityChanged)
            {
                sceneChainCanvas.repaint();
                return;
            }

            processor.setSceneChainStepTransitionLengthBeats(hitTransition, newLength);
            processor.setSceneChainStepTransitionIntensity(hitTransition, newIntensity);
            refreshFromProcessor();
        }
        return;
    }

    if (sceneChainDragSourceStep < 0)
        return;

    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    const int chainLength = processor.getSceneChainLength();
    int hitStep = sceneChainStepAtPosition(layout, e.position);
    if (hitStep < 0)
        hitStep = chainLength;
    hitStep = juce::jlimit(0, juce::jmin(MlrVSTAudioProcessor::MaxSceneChainSteps - 1, chainLength), hitStep);

    if (e.getDistanceFromDragStart() > 3)
        sceneChainDragMoved = true;

    if (hitStep != sceneChainDragTargetStep)
    {
        sceneChainDragTargetStep = hitStep;
        sceneChainCanvas.repaint();
    }
}

void SceneControlPanel::handleSceneChainMouseMove(const juce::MouseEvent& e)
{
    if (sceneChainDragTransition >= 0)
        return;

    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    const int hitStep = sceneChainStepAtPosition(layout, e.position);
    const int hitTransition = sceneChainTransitionAtPosition(layout, e.position);
    if (hitStep != sceneChainHoverStep || hitTransition != sceneChainHoverTransition)
    {
        sceneChainHoverStep = hitStep;
        sceneChainHoverTransition = hitTransition;
        sceneChainCanvas.repaint();
    }
}

void SceneControlPanel::handleSceneChainMouseExit(const juce::MouseEvent&)
{
    if (sceneChainDragTransition >= 0)
        return;

    if (sceneChainHoverStep >= 0 || sceneChainHoverTransition >= 0)
    {
        sceneChainHoverStep = -1;
        sceneChainHoverTransition = -1;
        sceneChainCanvas.repaint();
    }
}

void SceneControlPanel::handleSceneChainMouseUp(const juce::MouseEvent& e)
{
    const int dragTransition = sceneChainDragTransition;
    const bool transitionDragMoved = sceneChainDragTransitionMoved;
    sceneChainDragTransition = -1;
    sceneChainDragTransitionMoved = false;
    if (dragTransition >= 0)
    {
        const int chainLength = processor.getSceneChainLength();
        if (dragTransition >= 0 && dragTransition < chainLength)
        {
            selectedSceneChainStep = dragTransition;
            if (!transitionDragMoved)
            {
                auto transitionType = processor.getSceneChainStepTransitionType(dragTransition);
                if (e.mods.isAltDown())
                {
                    transitionType = MlrVSTAudioProcessor::SceneChainTransitionType::None;
                }
                else if (e.mods.isRightButtonDown() || e.mods.isCtrlDown())
                {
                    transitionType = previousSceneChainTransitionType(transitionType);
                }
                else
                {
                    transitionType = nextSceneChainTransitionType(transitionType);
                }

                processor.setSceneChainStepTransitionType(dragTransition, transitionType);
                if (sceneTransitionSmartLinkEnabled
                    && transitionType != MlrVSTAudioProcessor::SceneChainTransitionType::None
                    && transitionType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
                {
                    applySceneTransitionSmartTuning(dragTransition,
                                                    transitionType,
                                                    processor.getSceneChainStepTransitionOption(dragTransition),
                                                    processor.getSceneChainStepTransitionIntensity(dragTransition),
                                                    processor.getSceneChainStepTransitionLengthBeats(dragTransition),
                                                    processor.getSceneChainStepTransitionSubtractsFromSceneLength(dragTransition),
                                                    false,
                                                    false);
                    return;
                }
            }

            refreshFromProcessor();
            return;
        }
    }

    const int sourceStep = sceneChainDragSourceStep;
    const int targetStep = sceneChainDragTargetStep;
    const bool dragMoved = sceneChainDragMoved;
    sceneChainDragSourceStep = -1;
    sceneChainDragTargetStep = -1;
    sceneChainDragMoved = false;

    auto chainSteps = snapshotSceneChainSteps(processor);
    const int chainLength = static_cast<int>(chainSteps.size());
    const bool loopEnabled = processor.isSceneChainLoopEnabled();
    const int loopStart = processor.getSceneChainLoopStartStep();
    const int loopEnd = processor.getSceneChainLoopEndStep();

    if (sourceStep >= 0 && sourceStep < chainLength
        && dragMoved
        && targetStep >= 0
        && targetStep != sourceStep)
    {
        auto movedStep = chainSteps[static_cast<size_t>(sourceStep)];
        chainSteps.erase(chainSteps.begin() + sourceStep);
        int insertIndex = juce::jlimit(0, static_cast<int>(chainSteps.size()), targetStep);
        if (targetStep > sourceStep)
            insertIndex = juce::jlimit(0, static_cast<int>(chainSteps.size()), targetStep - 1);
        chainSteps.insert(chainSteps.begin() + insertIndex, movedStep);
        applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
        selectedSceneChainStep = insertIndex;
        refreshFromProcessor();
    }
    else if (sourceStep >= 0 && sourceStep < chainLength && !dragMoved)
    {
        const int sceneSlot = chainSteps[static_cast<size_t>(sourceStep)].sceneSlot;
        selectedSceneChainStep = sourceStep;
        if (sceneSlot >= 0 && sceneSlot < MlrVSTAudioProcessor::SceneSlots)
        {
            selectedSceneActionSlot = sceneSlot;
            processor.focusSceneSlot(sceneSlot);
        }
        refreshFromProcessor();
    }
    else
    {
        sceneChainCanvas.repaint();
    }
}

void SceneControlPanel::handleSceneChainMouseWheel(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
    const int chainLength = processor.getSceneChainLength();
    const float delta = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    if (std::abs(delta) < 1.0e-4f)
        return;

    int hitTransition = sceneChainTransitionAtPosition(layout, e.position);
    if (processor.isSceneChainLoopEnabled() && chainLength >= 2)
    {
        const auto loopClampBounds = sceneChainLoopClampBounds(layout,
                                                               processor.getSceneChainLoopStartStep(),
                                                               processor.getSceneChainLoopEndStep());
        if (loopClampBounds.expanded(4.0f, 6.0f).contains(e.position))
            hitTransition = chainLength - 1;
    }

    if (hitTransition >= 0 && hitTransition < chainLength)
    {
        selectedSceneChainStep = hitTransition;
        const bool increase = delta > 0.0f;
        const bool shiftDown = e.mods.isShiftDown();
        const bool altDown = e.mods.isAltDown();
        const bool commandOrCtrlDown = e.mods.isCommandDown() || e.mods.isCtrlDown();
        const auto currentType = processor.getSceneChainStepTransitionType(hitTransition);
        const auto currentOption = processor.getSceneChainStepTransitionOption(hitTransition);
        const auto currentScope = processor.getSceneChainStepTransitionScope(hitTransition);
        const auto currentContour = processor.getSceneChainStepTransitionContour(hitTransition);
        const auto currentCondition = processor.getSceneChainStepTransitionCondition(hitTransition);
        const float currentLength = processor.getSceneChainStepTransitionLengthBeats(hitTransition);
        const float currentIntensity = processor.getSceneChainStepTransitionIntensity(hitTransition);
        const bool subtractFromSceneLength = processor.getSceneChainStepTransitionSubtractsFromSceneLength(hitTransition);

        const bool linkedType = currentType != MlrVSTAudioProcessor::SceneChainTransitionType::None
            && currentType != MlrVSTAudioProcessor::SceneChainTransitionType::Return;
        const bool canUseLinkedRetune = sceneTransitionSmartLinkEnabled && linkedType;

        if (shiftDown && commandOrCtrlDown)
        {
            const auto newScope = increase
                ? nextSceneChainTransitionScope(currentScope)
                : previousSceneChainTransitionScope(currentScope);
            if (newScope == currentScope)
                return;

            processor.setSceneChainStepTransitionScope(hitTransition, newScope);
            refreshFromProcessor();
            return;
        }

        if (shiftDown && altDown)
        {
            const auto newContour = increase
                ? nextSceneChainTransitionContour(currentContour)
                : previousSceneChainTransitionContour(currentContour);
            if (newContour == currentContour)
                return;

            processor.setSceneChainStepTransitionContour(hitTransition, newContour);
            refreshFromProcessor();
            return;
        }

        if (commandOrCtrlDown && altDown)
        {
            const auto newCondition = increase
                ? nextSceneChainTransitionCondition(currentCondition)
                : previousSceneChainTransitionCondition(currentCondition);
            if (newCondition == currentCondition)
                return;

            processor.setSceneChainStepTransitionCondition(hitTransition, newCondition);
            refreshFromProcessor();
            return;
        }

        if (shiftDown)
        {
            const float newLength = steppedSceneTransitionLengthBeats(currentLength, increase);
            if (std::abs(newLength - currentLength) <= 1.0e-4f)
                return;

            if (canUseLinkedRetune)
                applySceneTransitionSmartTuning(hitTransition,
                                                currentType,
                                                currentOption,
                                                currentIntensity,
                                                newLength,
                                                subtractFromSceneLength,
                                                false,
                                                true);
            else
            {
                processor.setSceneChainStepTransitionLengthBeats(hitTransition, newLength);
                refreshFromProcessor();
            }
            return;
        }

        if (commandOrCtrlDown)
        {
            const float newIntensity = juce::jlimit(0.0f,
                                                    1.0f,
                                                    currentIntensity + (increase ? 0.08f : -0.08f));
            if (std::abs(newIntensity - currentIntensity) <= 1.0e-4f)
                return;

            if (canUseLinkedRetune)
                applySceneTransitionSmartTuning(hitTransition,
                                                currentType,
                                                currentOption,
                                                newIntensity,
                                                currentLength,
                                                subtractFromSceneLength,
                                                false,
                                                false);
            else
            {
                processor.setSceneChainStepTransitionIntensity(hitTransition, newIntensity);
                refreshFromProcessor();
            }
            return;
        }

        if (altDown)
        {
            auto nextType = increase
                ? nextSceneChainTransitionType(currentType)
                : previousSceneChainTransitionType(currentType);
            processor.setSceneChainStepTransitionType(hitTransition, nextType);
            if (sceneTransitionSmartLinkEnabled
                && nextType != MlrVSTAudioProcessor::SceneChainTransitionType::None
                && nextType != MlrVSTAudioProcessor::SceneChainTransitionType::Return)
            {
                applySceneTransitionSmartTuning(hitTransition,
                                                nextType,
                                                currentOption,
                                                currentIntensity,
                                                currentLength,
                                                subtractFromSceneLength,
                                                false,
                                                false);
                return;
            }
            refreshFromProcessor();
            return;
        }

        auto option = increase
            ? nextSceneChainTransitionOption(currentOption)
            : previousSceneChainTransitionOption(currentOption);
        processor.setSceneChainStepTransitionOption(hitTransition, option);
        if (canUseLinkedRetune)
        {
            applySceneTransitionSmartTuning(hitTransition,
                                            currentType,
                                            option,
                                            currentIntensity,
                                            currentLength,
                                            subtractFromSceneLength,
                                            false,
                                            false);
            return;
        }
        refreshFromProcessor();
        return;
    }

    const int hitStep = sceneChainStepAtPosition(layout, e.position);
    auto chainSteps = snapshotSceneChainSteps(processor);
    if (hitStep < 0 || hitStep >= static_cast<int>(chainSteps.size()))
        return;

    auto& step = chainSteps[static_cast<size_t>(hitStep)];
    const int newRepeats = juce::jlimit(1,
                                        MlrVSTAudioProcessor::MaxSceneRepeatCount,
                                        step.repeats + (delta > 0.0f ? 1 : -1));
    if (newRepeats == step.repeats)
        return;

    const bool loopEnabled = processor.isSceneChainLoopEnabled();
    const int loopStart = processor.getSceneChainLoopStartStep();
    const int loopEnd = processor.getSceneChainLoopEndStep();
    step.repeats = newRepeats;
    applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
    refreshFromProcessor();
}

bool SceneControlPanel::applyEditedSceneEvents(std::vector<ScenePerformanceEvent> events,
                                               int preferredSelectedIndex,
                                               const ScenePerformanceEvent* preferredSelectedEvent,
                                               const std::vector<ScenePerformanceEvent>* preferredSelectedEvents)
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const auto previousSelectedEvents = getSelectedSceneEvents();
    ensureSceneDefaultAutomationStartPoints(processor, events);
    if (!processor.replaceScenePerformanceClipEvents(sceneSlot, events))
        return false;

    sceneEditorState.events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    sceneEditorState.selectedSceneSlot = sceneSlot;
    if (sceneEditorState.events.empty())
    {
        clearSceneSelection();
    }
    else
    {
        std::vector<int> restoredSelection;
        int restoredPrimaryIndex = -1;
        const auto* selectionSource = preferredSelectedEvents;
        if (selectionSource == nullptr && !previousSelectedEvents.empty())
            selectionSource = &previousSelectedEvents;

        if (selectionSource != nullptr)
        {
            for (const auto& selectedEvent : *selectionSource)
            {
                const int matchIndex = findBestSceneEditorEventIndex(sceneEditorState.events, selectedEvent);
                if (matchIndex < 0)
                    continue;
                restoredSelection.push_back(matchIndex);
                if (preferredSelectedEvent != nullptr && restoredPrimaryIndex < 0)
                {
                    const int primaryMatch = findBestSceneEditorEventIndex(sceneEditorState.events, *preferredSelectedEvent);
                    if (primaryMatch >= 0)
                        restoredPrimaryIndex = primaryMatch;
                }
            }
        }

        if (restoredSelection.empty() && preferredSelectedEvent != nullptr)
        {
            const int matchIndex = findBestSceneEditorEventIndex(sceneEditorState.events, *preferredSelectedEvent);
            if (matchIndex >= 0)
            {
                restoredSelection.push_back(matchIndex);
                restoredPrimaryIndex = matchIndex;
            }
        }

        if (restoredSelection.empty() && preferredSelectedIndex >= 0 && preferredSelectedIndex < static_cast<int>(sceneEditorState.events.size()))
        {
            restoredSelection.push_back(preferredSelectedIndex);
            restoredPrimaryIndex = preferredSelectedIndex;
        }

        if (restoredSelection.empty() && sceneEditorState.selectedEventIndex >= 0)
        {
            const int fallbackIndex = juce::jlimit(0,
                                                   static_cast<int>(sceneEditorState.events.size()) - 1,
                                                   sceneEditorState.selectedEventIndex);
            restoredSelection.push_back(fallbackIndex);
            restoredPrimaryIndex = fallbackIndex;
        }

        setSceneSelectionIndices(std::move(restoredSelection), restoredPrimaryIndex);
    }

    refreshFromProcessor();
    return true;
}

bool SceneControlPanel::deleteSelectedSceneEvent()
{
    if (sceneEditorState.selectedEventIndices.empty())
    {
        return false;
    }

    auto events = sceneEditorState.events;
    const int nextIndex = sceneEditorState.selectedEventIndex;
    auto selectedIndices = sceneEditorState.selectedEventIndices;
    std::sort(selectedIndices.begin(), selectedIndices.end(), std::greater<int>());
    for (const int index : selectedIndices)
    {
        if (index < 0 || index >= static_cast<int>(events.size()))
            continue;
        events.erase(events.begin() + index);
    }

    if (events.empty())
    {
        const bool applied = applyEditedSceneEvents(std::move(events));
        if (applied)
            clearSceneSelection();
        return applied;
    }

    const int clampedNextIndex = juce::jlimit(0, static_cast<int>(events.size()) - 1, nextIndex);
    clearSceneSelection();
    return applyEditedSceneEvents(std::move(events), clampedNextIndex, nullptr, nullptr);
}

bool SceneControlPanel::duplicateSelectedSceneEvents()
{
    if (sceneEditorState.selectedEventIndices.empty())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const double stepBeats = 4.0 / static_cast<double>(juce::jlimit(1, 64, sceneGridDivision));
    auto events = sceneEditorState.events;
    auto selectedEvents = getSelectedSceneEvents();
    std::vector<ScenePerformanceEvent> duplicatedEvents;
    duplicatedEvents.reserve(selectedEvents.size());
    for (auto event : selectedEvents)
    {
        event.timeBeats = snapSceneBeatToGrid(event.timeBeats + stepBeats, lengthBeats);
        duplicatedEvents.push_back(event);
        events.push_back(event);
    }

    if (duplicatedEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events), -1, &duplicatedEvents.front(), &duplicatedEvents);
}

bool SceneControlPanel::nudgeSelectedSceneEvents(int direction)
{
    if (sceneEditorState.selectedEventIndices.empty() || direction == 0)
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const double stepBeats = 4.0 / static_cast<double>(juce::jlimit(1, 64, sceneGridDivision));
    auto events = sceneEditorState.events;
    auto selectedIndices = sceneEditorState.selectedEventIndices;
    std::vector<ScenePerformanceEvent> nudgedEvents;
    nudgedEvents.reserve(selectedIndices.size());
    for (const int index : selectedIndices)
    {
        if (index < 0 || index >= static_cast<int>(events.size()))
            continue;
        auto& event = events[static_cast<size_t>(index)];
        event.timeBeats = snapSceneBeatToGrid(event.timeBeats + (stepBeats * static_cast<double>(direction)), lengthBeats);
        nudgedEvents.push_back(event);
    }

    if (nudgedEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events), -1, &nudgedEvents.front(), &nudgedEvents);
}

bool SceneControlPanel::quantizeSelectedSceneEvents()
{
    if (sceneEditorState.selectedEventIndices.empty())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    auto events = sceneEditorState.events;
    auto selectedIndices = sceneEditorState.selectedEventIndices;
    std::vector<ScenePerformanceEvent> quantizedEvents;
    quantizedEvents.reserve(selectedIndices.size());
    for (const int index : selectedIndices)
    {
        if (index < 0 || index >= static_cast<int>(events.size()))
            continue;
        auto& event = events[static_cast<size_t>(index)];
        event.timeBeats = snapSceneBeatToGrid(event.timeBeats, lengthBeats);
        quantizedEvents.push_back(event);
    }

    if (quantizedEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events), -1, &quantizedEvents.front(), &quantizedEvents);
}

bool SceneControlPanel::clearStripSceneEvents(int stripIndex)
{
    auto events = sceneEditorState.events;
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    const int sceneSlot = getFocusedSceneSlot();
    bool removedAnyEvent = false;
    bool removedControlPoint = false;
    std::vector<ScenePerformanceControlTarget> clearedTargets;
    for (const auto& event : events)
    {
        if (event.stripIndex != safeStripIndex)
            continue;

        removedAnyEvent = true;
        if (event.type != ScenePerformanceEventType::ControlPoint)
            continue;

        removedControlPoint = true;
        if (std::find(clearedTargets.begin(), clearedTargets.end(), event.controlTarget) == clearedTargets.end())
            clearedTargets.push_back(event.controlTarget);
    }

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeStripIndex](const ScenePerformanceEvent& event)
                                {
                                    return event.stripIndex == safeStripIndex;
                                }),
                 events.end());

    if (removedControlPoint)
        appendSceneStripStoredDefaultAutomationStartPoints(processor, sceneSlot, safeStripIndex, events);

    processor.clearSceneMotionStripState(sceneSlot, safeStripIndex);
    if (!removedAnyEvent)
    {
        refreshFromProcessor();
        return true;
    }

    const bool applied = applyEditedSceneEvents(std::move(events));
    if (applied)
    {
        processor.restoreSceneStripControlTargetsToStoredState(sceneSlot, safeStripIndex, clearedTargets);
        refreshFromProcessor();
    }

    return applied;
}

bool SceneControlPanel::writeCurrentStripAutomation(int stripIndex)
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    if (!processor.isStripScenePlaybackAvailable(safeStripIndex))
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    auto events = sceneEditorState.events;
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeStripIndex](const ScenePerformanceEvent& event)
                                {
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && event.stripIndex == safeStripIndex;
                                }),
                 events.end());

    std::vector<ScenePerformanceEvent> writtenEvents;
    writtenEvents.reserve(static_cast<size_t>(kSceneAutomationLaneCount) * 2u);
    appendSceneStripAutomationWriteEvents(processor, safeStripIndex, lengthBeats, writtenEvents);

    events.insert(events.end(), writtenEvents.begin(), writtenEvents.end());
    return applyEditedSceneEvents(std::move(events));
}

bool SceneControlPanel::writeAllStripsAutomation()
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const int visibleStripCount = getVisibleSceneStripCount();
    auto events = sceneEditorState.events;
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [visibleStripCount](const ScenePerformanceEvent& event)
                                {
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && ((event.stripIndex >= 0 && event.stripIndex < visibleStripCount)
                                            || sceneIsGlobalAutomationEvent(event));
                                }),
                 events.end());

    std::vector<ScenePerformanceEvent> writtenEvents;
    writtenEvents.reserve(static_cast<size_t>((visibleStripCount * kSceneAutomationLaneCount * 2) + 2));
    for (int stripIndex = 0; stripIndex < visibleStripCount; ++stripIndex)
    {
        if (!processor.isStripScenePlaybackAvailable(stripIndex))
            continue;
        appendSceneStripAutomationWriteEvents(processor, stripIndex, lengthBeats, writtenEvents);
    }
    appendSceneGlobalAutomationWriteEvents(processor, lengthBeats, writtenEvents);

    if (writtenEvents.empty())
        return false;

    events.insert(events.end(), writtenEvents.begin(), writtenEvents.end());
    return applyEditedSceneEvents(std::move(events));
}

bool SceneControlPanel::copyStripSceneEvents(int sourceStripIndex, int destStripIndex)
{
    const int safeSourceStrip = juce::jlimit(0, getVisibleSceneStripCount() - 1, sourceStripIndex);
    const int safeDestStrip = juce::jlimit(0, getVisibleSceneStripCount() - 1, destStripIndex);
    if (safeSourceStrip == safeDestStrip)
        return false;
    if (!processor.isStripScenePlaybackAvailable(safeSourceStrip)
        || !processor.isStripScenePlaybackAvailable(safeDestStrip))
    {
        return false;
    }

    processor.copySceneMotionStripState(getFocusedSceneSlot(), safeSourceStrip, safeDestStrip);

    auto events = sceneEditorState.events;
    std::vector<ScenePerformanceEvent> copiedEvents;
    copiedEvents.reserve(events.size());
    for (const auto& event : sceneEditorState.events)
    {
        if (event.stripIndex != safeSourceStrip)
            continue;
        auto copiedEvent = event;
        copiedEvent.stripIndex = safeDestStrip;
        copiedEvents.push_back(copiedEvent);
    }

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeDestStrip](const ScenePerformanceEvent& event)
                                {
                                    return event.stripIndex == safeDestStrip;
                                }),
                 events.end());
    events.insert(events.end(), copiedEvents.begin(), copiedEvents.end());
    if (copiedEvents.empty())
        return applyEditedSceneEvents(std::move(events));

    return applyEditedSceneEvents(std::move(events), -1, &copiedEvents.front(), &copiedEvents);
}

bool SceneControlPanel::duplicateStripSceneEventsToNext(int stripIndex)
{
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    if (safeStripIndex >= getVisibleSceneStripCount() - 1)
        return false;

    return copyStripSceneEvents(safeStripIndex, safeStripIndex + 1);
}

bool SceneControlPanel::duplicateFocusedSceneLength()
{
    if (processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = getFocusedSceneSlot();
    const int currentCount = processor.getSceneLengthCount(sceneSlot);
    if (currentCount <= 0 || currentCount > (MlrVSTAudioProcessor::MaxSceneManualBars / 2))
        return false;

    const double originalLengthBeats = juce::jmax(1.0, processor.getResolvedSceneLengthBeats(sceneSlot));
    auto events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    std::vector<ScenePerformanceEvent> duplicatedEvents;
    duplicatedEvents.reserve(events.size() * 2);
    duplicatedEvents.insert(duplicatedEvents.end(), events.begin(), events.end());
    for (const auto& event : events)
    {
        auto copy = event;
        copy.timeBeats += originalLengthBeats;
        duplicatedEvents.push_back(copy);
    }

    processor.setSceneLengthCount(sceneSlot, currentCount * 2);
    processor.persistSceneTimingForSlot(sceneSlot);
    if (!processor.replaceScenePerformanceClipEvents(sceneSlot, duplicatedEvents))
        return false;

    sceneEditorState.events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    clearSceneSelection();
    updateSceneTimelineContentSize();
    sceneTimelineCanvas.repaint();
    return true;
}

int SceneControlPanel::defaultStepTriggerColumnForStrip(int stripIndex) const
{
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    if (sceneEditorState.selectedSceneSlot == getFocusedSceneSlot())
    {
        for (int index : sceneEditorState.selectedEventIndices)
        {
            if (index < 0 || index >= static_cast<int>(sceneEditorState.events.size()))
                continue;

            const auto& event = sceneEditorState.events[static_cast<size_t>(index)];
            if (event.type == ScenePerformanceEventType::Trigger && event.stripIndex == safeStripIndex)
                return juce::jlimit(0, kSceneStepColumnsPerRow - 1, event.column);
        }
    }

    const bool focusedSceneIsActive = getFocusedSceneSlot() == processor.getActiveSceneSlot();
    auto* engine = focusedSceneIsActive ? processor.getAudioEngine() : nullptr;
    auto* strip = engine != nullptr ? engine->getStrip(safeStripIndex) : nullptr;
    if (strip != nullptr)
        return juce::jlimit(0, kSceneStepColumnsPerRow - 1, strip->currentStep % kSceneStepColumnsPerRow);

    return 0;
}

bool SceneControlPanel::assignSelectedStepTriggerColumn(int stripIndex, int column)
{
    const int safeStripIndex = juce::jlimit(0, getVisibleSceneStripCount() - 1, stripIndex);
    const int safeColumn = juce::jlimit(0, kSceneStepColumnsPerRow - 1, column);
    if (sceneEditorState.selectedSceneSlot != getFocusedSceneSlot())
        return false;

    auto events = sceneEditorState.events;
    std::vector<ScenePerformanceEvent> updatedEvents;
    updatedEvents.reserve(sceneEditorState.selectedEventIndices.size());

    for (int index : sceneEditorState.selectedEventIndices)
    {
        if (index < 0 || index >= static_cast<int>(events.size()))
            continue;

        auto& event = events[static_cast<size_t>(index)];
        if (event.type != ScenePerformanceEventType::Trigger || event.stripIndex != safeStripIndex)
            continue;

        if (event.column != safeColumn)
        {
            event.column = safeColumn;
            event.sampleSliceId = -1;
            event.sampleStartSample = -1;
        }
        updatedEvents.push_back(event);
    }

    if (updatedEvents.empty())
        return false;

    return applyEditedSceneEvents(std::move(events),
                                  -1,
                                  &updatedEvents.front(),
                                  &updatedEvents);
}

bool SceneControlPanel::trimSceneEventsToSelection(bool keepAfterSelection)
{
    if (sceneEditorState.selectedEventIndex < 0
        || sceneEditorState.selectedEventIndex >= static_cast<int>(sceneEditorState.events.size()))
    {
        return false;
    }

    const auto selectedEvent = sceneEditorState.events[static_cast<size_t>(sceneEditorState.selectedEventIndex)];
    auto events = sceneEditorState.events;
    const double pivotBeat = selectedEvent.timeBeats;
    const double epsilon = 1.0e-6;

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [keepAfterSelection, pivotBeat, epsilon](const ScenePerformanceEvent& event)
                                {
                                    return keepAfterSelection
                                        ? (event.timeBeats + epsilon) < pivotBeat
                                        : (event.timeBeats - epsilon) > pivotBeat;
                                }),
                 events.end());

    return applyEditedSceneEvents(std::move(events), -1, &selectedEvent);
}

bool SceneControlPanel::clearSceneEventsByType(ScenePerformanceEventType type)
{
    auto events = sceneEditorState.events;
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [type](const ScenePerformanceEvent& event)
                                {
                                    return event.type == type;
                                }),
                 events.end());

    const ScenePerformanceEvent* selectedEvent = nullptr;
    ScenePerformanceEvent selectedEventCopy;
    if (sceneEditorState.selectedEventIndex >= 0
        && sceneEditorState.selectedEventIndex < static_cast<int>(sceneEditorState.events.size()))
    {
        selectedEventCopy = sceneEditorState.events[static_cast<size_t>(sceneEditorState.selectedEventIndex)];
        if (selectedEventCopy.type != type)
            selectedEvent = &selectedEventCopy;
    }

    return applyEditedSceneEvents(std::move(events), -1, selectedEvent);
}

bool SceneControlPanel::handleEditorKeyPress(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return deleteSelectedSceneEvent();

    if (key == juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0)
        || key == juce::KeyPress('c', juce::ModifierKeys::ctrlModifier, 0))
    {
        const bool copied = processor.copyScenePerformanceClipToClipboard(getFocusedSceneSlot());
        if (copied)
            refreshFromProcessor();
        return copied;
    }

    if (key == juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)
        || key == juce::KeyPress('v', juce::ModifierKeys::ctrlModifier, 0))
    {
        const bool pasted = processor.pasteScenePerformanceClipFromClipboard(getFocusedSceneSlot());
        if (pasted)
            refreshFromProcessor();
        return pasted;
    }

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)
        || key == juce::KeyPress('a', juce::ModifierKeys::ctrlModifier, 0))
    {
        std::vector<int> allIndices(sceneEditorState.events.size());
        std::iota(allIndices.begin(), allIndices.end(), 0);
        setSceneSelectionIndices(std::move(allIndices), sceneEditorState.events.empty() ? -1 : 0);
        refreshFromProcessor();
        return true;
    }

    if (key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)
        || key == juce::KeyPress('d', juce::ModifierKeys::ctrlModifier, 0))
        return duplicateSelectedSceneEvents();

    if (key.getTextCharacter() == 'q' || key.getTextCharacter() == 'Q')
        return quantizeSelectedSceneEvents();

    if (key == juce::KeyPress::leftKey)
        return nudgeSelectedSceneEvents(-1);

    if (key == juce::KeyPress::rightKey)
        return nudgeSelectedSceneEvents(1);

    if (key == juce::KeyPress::escapeKey)
    {
        if (sceneEndSampleEditorVisible)
        {
            closeSceneTransitionEndSampleEditor();
            return true;
        }

        if (sceneLegacyModEditorVisible)
        {
            closeLegacyModEditor();
            return true;
        }

        if (!sceneEditorState.selectedEventIndices.empty())
        {
            clearSceneSelection();
            refreshFromProcessor();
            return true;
        }
    }

    return false;
}

void SceneControlPanel::paintSceneTimelineCanvas(juce::Graphics& g) const
{
    auto bounds = sceneTimelineCanvas.getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    g.fillAll(juce::Colours::transparentBlack);

    const int sceneSlot = getFocusedSceneSlot();
    const bool focusedSceneIsActive = sceneSlot == processor.getActiveSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const juce::String sceneLengthLabel = sceneLengthSummaryLabel(processor, sceneSlot);
    const bool mainModSceneLaneSelectionActive = processor.isSceneModeEnabled()
        && processor.isControlModeActive()
        && processor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation
        && processor.getSceneModPageMode() == MlrVSTAudioProcessor::SceneModPageMode::MainModulation
        && processor.getAudioEngine() != nullptr;
    const int mainModSceneStripIndex = mainModSceneLaneSelectionActive
        ? juce::jlimit(0,
                       juce::jmax(0, getVisibleSceneStripCount() - 1),
                       processor.getLastMonomePressedStripRow())
        : -1;
    const auto mainModSceneTarget = mainModSceneLaneSelectionActive
        ? processor.getSceneMainAutomationDisplayTargetForStrip(mainModSceneStripIndex)
        : ModernAudioEngine::ModTarget::None;
    auto laneIsModSelected = [this, mainModSceneLaneSelectionActive, mainModSceneStripIndex, mainModSceneTarget]
        (int stripIndex, int laneIndex) -> bool
    {
        if (sceneLegacyModEditorVisible
            && sceneLegacyModSlotIndex >= 0
            && sceneLegacyModStripIndex == stripIndex)
        {
            return sceneLaneMatchesModTarget(processor,
                                             stripIndex,
                                             laneIndex,
                                             processor.getSceneMotionTargetForSlot(stripIndex, sceneLegacyModSlotIndex));
        }

        return mainModSceneLaneSelectionActive
            && mainModSceneStripIndex == stripIndex
            && mainModSceneTarget != ModernAudioEngine::ModTarget::None
            && sceneLaneMatchesModTarget(processor, stripIndex, laneIndex, mainModSceneTarget);
    };
    const int globalLane = sceneGlobalAutomationLaneIndex();
    if (globalLane >= 0)
    {
        const auto globalLayout = makeSceneGlobalLaneLayout(
            juce::Rectangle<float>(0.0f,
                                   0.0f,
                                   bounds.getWidth(),
                                   sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
            sceneGlobalLaneExpanded);
        ScenePerformanceEvent globalProbe;
        globalProbe.type = ScenePerformanceEventType::ControlPoint;
        globalProbe.controlTarget = ScenePerformanceControlTarget::Retrigger;
        const auto globalColour = sceneAutomationColour(globalProbe);

        int globalEventCount = 0;
        for (const auto& event : sceneEditorState.events)
        {
            if (sceneIsGlobalAutomationEvent(event))
                ++globalEventCount;
        }

        drawPanel(g, globalLayout.cardBounds.reduced(1.0f), globalColour, 8.0f);
        g.setColour(globalColour.withAlpha(0.9f));
        g.fillRoundedRectangle(globalLayout.cardBounds.withWidth(3.0f).reduced(0.0f, 6.0f), 1.5f);

        g.setColour(globalColour);
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText("GLOBAL",
                   globalLayout.titleBounds.toNearestInt(),
                   juce::Justification::centredLeft);

        g.setColour(kTextSecondary);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        g.drawText(juce::String(globalEventCount) + " ctrl  •  all strips  •  " + sceneLengthLabel,
                   globalLayout.summaryBounds.toNearestInt(),
                   juce::Justification::centredRight);

        g.setColour(kTextSecondary);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(sceneAutomationLaneName(globalLane),
                   globalLayout.laneLabelBounds.toNearestInt(),
                   juce::Justification::centredLeft);

        g.setColour(kSurfaceDark.brighter(0.04f));
        g.fillRoundedRectangle(globalLayout.laneBounds, 4.0f);
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.drawRoundedRectangle(globalLayout.laneBounds.reduced(0.5f), 4.0f, 1.0f);

        const auto scaleLabels = sceneAutomationLaneScaleLabels(globalLane);
        if (scaleLabels[0].isNotEmpty() || scaleLabels[1].isNotEmpty() || scaleLabels[2].isNotEmpty())
        {
            g.setColour(kTextMuted.withAlpha(0.82f));
            g.setFont(juce::Font(juce::FontOptions(8.0f)));
            g.drawText(scaleLabels[0],
                       juce::Rectangle<int>(static_cast<int>(globalLayout.laneBounds.getX() + 4.0f),
                                            static_cast<int>(globalLayout.laneBounds.getY() + 1.0f),
                                            28,
                                            9),
                       juce::Justification::centredLeft,
                       false);
            g.drawText(scaleLabels[1],
                       juce::Rectangle<int>(static_cast<int>(globalLayout.laneBounds.getX() + 4.0f),
                                            static_cast<int>(std::round(globalLayout.laneBounds.getCentreY() - 5.0f)),
                                            28,
                                            10),
                       juce::Justification::centredLeft,
                       false);
            g.drawText(scaleLabels[2],
                       juce::Rectangle<int>(static_cast<int>(globalLayout.laneBounds.getX() + 4.0f),
                                            static_cast<int>(globalLayout.laneBounds.getBottom() - 10.0f),
                                            28,
                                            9),
                       juce::Justification::centredLeft,
                       false);
        }

        const int safeGridDivision = juce::jlimit(4, 32, sceneGridDivision);
        const bool drawGridVisible = sceneGridEnabled || sceneDrawModeEnabled;
        const double gridStepBeats = drawGridVisible ? (4.0 / static_cast<double>(safeGridDivision)) : 0.0;
        const int barCount = juce::jmax(1, static_cast<int>(std::ceil(lengthBeats / 4.0)));
        for (int barIndex = 0; barIndex < barCount; ++barIndex)
        {
            if ((barIndex % 2) != 0)
                continue;

            const float x = globalLayout.laneBounds.getX()
                + (globalLayout.laneBounds.getWidth()
                   * static_cast<float>((barIndex * 4.0) / juce::jmax(1.0, lengthBeats)));
            const float nextX = globalLayout.laneBounds.getX()
                + (globalLayout.laneBounds.getWidth()
                   * static_cast<float>(((barIndex + 1) * 4.0) / juce::jmax(1.0, lengthBeats)));
            g.setColour(globalColour.withAlpha(0.05f));
            g.fillRect(juce::Rectangle<float>(x,
                                              globalLayout.laneBounds.getY() + 1.0f,
                                              juce::jmax(0.0f, juce::jmin(nextX, globalLayout.laneBounds.getRight()) - x),
                                              juce::jmax(0.0f, globalLayout.laneBounds.getHeight() - 2.0f)));
        }

        if (drawGridVisible && gridStepBeats > 0.0)
        {
            const int steps = juce::jmax(1, static_cast<int>(std::ceil(lengthBeats / gridStepBeats)));
            for (int step = 0; step <= steps; ++step)
            {
                const double stepBeat = juce::jlimit(0.0, lengthBeats, static_cast<double>(step) * gridStepBeats);
                if (std::abs(stepBeat - std::round(stepBeat)) < 1.0e-6)
                    continue;

                const float x = globalLayout.laneBounds.getX()
                    + (globalLayout.laneBounds.getWidth()
                       * static_cast<float>(stepBeat / juce::jmax(1.0, lengthBeats)));
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.drawVerticalLine(static_cast<int>(std::round(x)),
                                   globalLayout.laneBounds.getY() + 1.0f,
                                   globalLayout.laneBounds.getBottom() - 1.0f);
            }
        }

        if (sceneLaneOverlayEnabled)
        {
            g.setColour(globalColour.withAlpha(0.08f));
            g.fillRoundedRectangle(globalLayout.laneBounds.reduced(0.8f), 3.0f);
        }

        const bool hoverGlobalLane = sceneEditorState.hoverActive
            && !sceneEditorState.hoverTriggerLane
            && sceneEditorState.hoverStripIndex < 0
            && sceneEditorState.hoverLaneIndex == globalLane;
        if (hoverGlobalLane)
        {
            g.setColour(globalColour.withAlpha(0.12f));
            g.fillRoundedRectangle(globalLayout.laneBounds.reduced(0.8f), 3.0f);

            const float x = globalLayout.laneBounds.getX()
                + (globalLayout.laneBounds.getWidth()
                   * static_cast<float>(sceneEditorState.hoverBeat / juce::jmax(1.0, lengthBeats)));
            if (drawGridVisible && gridStepBeats > 0.0)
            {
                const double cellBeatStart = juce::jlimit(0.0,
                                                          juce::jmax(0.0, std::nextafter(lengthBeats, 0.0)),
                                                          sceneEditorState.hoverBeat);
                const double cellBeatEnd = juce::jlimit(0.0, lengthBeats, cellBeatStart + gridStepBeats);
                const float startX = globalLayout.laneBounds.getX()
                    + (globalLayout.laneBounds.getWidth()
                       * static_cast<float>(cellBeatStart / juce::jmax(1.0, lengthBeats)));
                const float endX = globalLayout.laneBounds.getX()
                    + (globalLayout.laneBounds.getWidth()
                       * static_cast<float>(cellBeatEnd / juce::jmax(1.0, lengthBeats)));
                g.setColour(globalColour.withAlpha(0.16f));
                g.fillRoundedRectangle(juce::Rectangle<float>(startX,
                                                              globalLayout.laneBounds.getY() + 1.0f,
                                                              juce::jmax(2.0f, endX - startX),
                                                              juce::jmax(2.0f, globalLayout.laneBounds.getHeight() - 2.0f)),
                                       2.6f);
            }

            g.setColour(globalColour.withAlpha(sceneDrawModeEnabled ? 0.92f : 0.72f));
            g.drawLine(x,
                       globalLayout.laneBounds.getY() + 1.0f,
                       x,
                       globalLayout.laneBounds.getBottom() - 1.0f,
                       sceneDrawModeEnabled ? 1.8f : 1.2f);
        }

        juce::Point<float> previousPoint;
        juce::Point<float> firstPoint;
        juce::Point<float> lastPoint;
        bool firstPointStepped = false;
        bool lastPointStepped = false;
        bool hasPreviousPoint = false;
        bool hasFirstPoint = false;
        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            const auto marker = sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats);
            const auto point = marker.getCentre();
            if (!hasFirstPoint)
            {
                firstPoint = point;
                firstPointStepped = sceneAutomationEventUsesSteppedSegments(event);
                hasFirstPoint = true;
            }
            if (hasPreviousPoint)
            {
                drawSceneAutomationConnection(g,
                                              previousPoint,
                                              point,
                                              globalColour.withAlpha(0.4f),
                                              sceneAutomationEventUsesSteppedSegments(event),
                                              1.2f);
            }
            previousPoint = point;
            lastPoint = point;
            lastPointStepped = sceneAutomationEventUsesSteppedSegments(event);
            hasPreviousPoint = true;
        }

        if (hasFirstPoint)
        {
            const auto connectionColour = globalColour.withAlpha(0.4f);
            drawSceneAutomationConnection(g,
                                          { globalLayout.laneBounds.getX(), firstPoint.y },
                                          firstPoint,
                                          connectionColour,
                                          firstPointStepped,
                                          1.2f);
            drawSceneAutomationConnection(g,
                                          lastPoint,
                                          { globalLayout.laneBounds.getRight(), lastPoint.y },
                                          connectionColour,
                                          lastPointStepped,
                                          1.2f);
        }

        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            const bool isSelected = isSceneEventIndexSelected(eventIndex)
                && sceneEditorState.selectedSceneSlot == sceneSlot;
            drawSceneAutomationPoint(g,
                                     event,
                                     sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats),
                                     isSelected);
        }

        if (globalEventCount == 0)
        {
            g.setColour(kTextMuted.withAlpha(0.42f));
            g.setFont(juce::Font(juce::FontOptions(8.4f)));
            g.drawText("Double-click to add " + sceneAutomationLaneCreateHint(globalLane),
                       globalLayout.laneBounds.toNearestInt(),
                       juce::Justification::centredRight);
        }

        if (sceneEditorState.transportProgress >= 0.0f)
        {
            const float headX = globalLayout.laneBounds.getX()
                + (globalLayout.laneBounds.getWidth() * sceneEditorState.transportProgress);
            const auto headColour = (sceneEditorState.transportRecording ? juce::Colour(0xffd46b62)
                                                                         : juce::Colour(0xff76be7e)).withAlpha(0.95f);
            g.setColour(headColour);
            g.drawLine(headX,
                       globalLayout.cardBounds.getY() + 2.0f,
                       headX,
                       globalLayout.cardBounds.getBottom() - 2.0f,
                       1.4f);
        }
    }

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);

    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const bool automationExpanded = stripAutomationExpanded[static_cast<size_t>(visibleStrip)];
        const float cardHeight = sceneStripCardHeight(processor,
                                                      visibleStrip,
                                                      automationExpanded,
                                                      stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        const auto cardBounds = juce::Rectangle<float>(0.0f, y, bounds.getWidth(), cardHeight);
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     automationExpanded,
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        const auto stripColour = getStripColor(visibleStrip);
        const bool stripIsSelected = visibleStrip
            == juce::jlimit(0,
                            juce::jmax(0, getVisibleSceneStripCount() - 1),
                            sceneEditorState.selectedStripIndex);
        const bool scenePlaybackAvailable = layout.scenePlaybackAvailable;

        drawPanel(g, cardBounds.reduced(1.0f), stripColour, 8.0f);
        if (stripIsSelected)
        {
            g.setColour(stripColour.withAlpha(layout.compactOverview ? 0.96f : 0.84f));
            g.drawRoundedRectangle(cardBounds.reduced(1.4f), 8.0f, layout.compactOverview ? 1.3f : 1.6f);
        }
        g.setColour(stripColour.withAlpha(0.9f));
        g.fillRoundedRectangle(cardBounds.withWidth(3.0f).reduced(0.0f, 6.0f), 1.5f);

        int triggerCount = 0;
        int controlCount = 0;
        for (const auto& event : sceneEditorState.events)
        {
            if (event.stripIndex != visibleStrip)
                continue;

            if (event.type == ScenePerformanceEventType::ControlPoint)
                ++controlCount;
            else
                ++triggerCount;
        }

        g.setColour(stripColour);
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText("STRIP " + juce::String(visibleStrip + 1),
                   layout.titleBounds.toNearestInt(),
                   juce::Justification::centredLeft);

        const bool stripHasEvents = (triggerCount + controlCount) > 0;
        drawSceneHeaderActionChip(g, layout.stripWriteBounds, "Write", stripColour, scenePlaybackAvailable);
        drawSceneHeaderActionChip(g, layout.stripWriteAllBounds, "All", stripColour, scenePlaybackAvailable);
        drawSceneHeaderActionChip(g, layout.stripClearBounds, "Clr", stripColour, stripHasEvents);
        drawSceneHeaderActionChip(g,
                                  layout.stripDuplicateBounds,
                                  "Dup",
                                  stripColour,
                                  scenePlaybackAvailable && stripHasEvents && visibleStrip < getVisibleSceneStripCount() - 1);
        drawSceneHeaderActionChip(g,
                                  layout.stripCopyBounds,
                                  "Copy",
                                  stripColour,
                                  scenePlaybackAvailable && stripHasEvents);

        auto summaryBounds = layout.summaryBounds;
        if (stripIsSelected)
        {
            auto selectedChipBounds = summaryBounds.removeFromRight(58.0f).reduced(0.0f, 1.0f);
            drawSceneHeaderActionChip(g, selectedChipBounds, "Selected", stripColour, true);
            summaryBounds.removeFromRight(6.0f);
        }
        g.setColour(kTextSecondary);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        g.drawText(scenePlaybackAvailable
                       ? (juce::String(triggerCount) + " trig  •  "
                          + juce::String(controlCount) + " ctrl  •  " + sceneLengthLabel
                          + (layout.compactOverview ? "  •  click edit" : ""))
                       : sceneStripPlaybackUnavailableReason(processor, visibleStrip),
                   summaryBounds.toNearestInt(),
                   juce::Justification::centredRight);

        const auto triggerTimeBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
            ? layout.stepLaunchBounds
            : layout.triggerTimelineBounds;
        auto timelineUnion = layout.rulerTimelineBounds.getUnion(triggerTimeBounds);
        if (layout.automationExpanded)
        {
            for (const auto& laneBounds : layout.automationTimelineBounds)
            {
                if (!laneBounds.isEmpty())
                    timelineUnion = timelineUnion.getUnion(laneBounds);
            }
        }

        g.setColour(kSurfaceDark.brighter(0.02f));
        g.fillRoundedRectangle(layout.rulerTimelineBounds, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawRoundedRectangle(layout.rulerTimelineBounds.reduced(0.5f), 3.0f, 1.0f);

        if (!layout.stepTriggerLane)
        {
            g.setColour(kSurfaceDark.brighter(0.04f));
            g.fillRoundedRectangle(layout.triggerTimelineBounds, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawRoundedRectangle(layout.triggerTimelineBounds.reduced(0.5f), 4.0f, 1.0f);
        }
        else if (!layout.stepLaunchBounds.isEmpty())
        {
            g.setColour(kSurfaceDark.brighter(0.04f));
            g.fillRoundedRectangle(layout.stepLaunchBounds, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawRoundedRectangle(layout.stepLaunchBounds.reduced(0.5f), 4.0f, 1.0f);
        }

        const int safeGridDivision = juce::jlimit(4, 32, sceneGridDivision);
        const bool drawGridVisible = sceneGridEnabled || sceneDrawModeEnabled;
        const double gridStepBeats = drawGridVisible ? (4.0 / static_cast<double>(safeGridDivision)) : 0.0;
        auto drawBarBlocks = [&](juce::Rectangle<float> timelineBounds)
        {
            if (timelineBounds.isEmpty())
                return;

            const int barCount = juce::jmax(1, static_cast<int>(std::ceil(lengthBeats / 4.0)));
            for (int barIndex = 0; barIndex < barCount; ++barIndex)
            {
                if ((barIndex % 2) != 0)
                    continue;

                const float x = timelineBounds.getX()
                    + (timelineBounds.getWidth() * static_cast<float>((barIndex * 4.0) / juce::jmax(1.0, lengthBeats)));
                const float nextX = timelineBounds.getX()
                    + (timelineBounds.getWidth() * static_cast<float>(((barIndex + 1) * 4.0) / juce::jmax(1.0, lengthBeats)));
                g.setColour(stripColour.withAlpha(0.05f));
                g.fillRect(juce::Rectangle<float>(x,
                                                  timelineBounds.getY() + 1.0f,
                                                  juce::jmax(0.0f, juce::jmin(nextX, timelineBounds.getRight()) - x),
                                                  juce::jmax(0.0f, timelineBounds.getHeight() - 2.0f)));
            }
        };

        auto drawSubGrid = [&](juce::Rectangle<float> timelineBounds)
        {
            if (!drawGridVisible || gridStepBeats <= 0.0 || timelineBounds.isEmpty())
                return;

            const int steps = juce::jmax(1, static_cast<int>(std::ceil(lengthBeats / gridStepBeats)));
            for (int step = 0; step <= steps; ++step)
            {
                const double stepBeat = juce::jlimit(0.0, lengthBeats, static_cast<double>(step) * gridStepBeats);
                if (std::abs(stepBeat - std::round(stepBeat)) < 1.0e-6)
                    continue;

                const float x = timelineBounds.getX()
                    + (timelineBounds.getWidth() * static_cast<float>(stepBeat / juce::jmax(1.0, lengthBeats)));
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.drawVerticalLine(static_cast<int>(std::round(x)),
                                   timelineBounds.getY() + 1.0f,
                                   timelineBounds.getBottom() - 1.0f);
            }
        };

        auto drawHoverOverlay = [&](juce::Rectangle<float> laneBounds, juce::Colour tint, bool laneMatchesHover)
        {
            if (sceneLaneOverlayEnabled)
            {
                g.setColour(tint.withAlpha(0.08f));
                g.fillRoundedRectangle(laneBounds.reduced(0.8f), 3.0f);
            }

            if (!laneMatchesHover)
                return;

            g.setColour(tint.withAlpha(0.12f));
            g.fillRoundedRectangle(laneBounds.reduced(0.8f), 3.0f);

            const float x = laneBounds.getX()
                + (laneBounds.getWidth() * static_cast<float>(sceneEditorState.hoverBeat / juce::jmax(1.0, lengthBeats)));
            if (drawGridVisible && gridStepBeats > 0.0)
            {
                const double cellBeatStart = juce::jlimit(0.0, juce::jmax(0.0, std::nextafter(lengthBeats, 0.0)), sceneEditorState.hoverBeat);
                const double cellBeatEnd = juce::jlimit(0.0,
                                                        lengthBeats,
                                                        cellBeatStart + gridStepBeats);
                const float startX = laneBounds.getX()
                    + (laneBounds.getWidth() * static_cast<float>(cellBeatStart / juce::jmax(1.0, lengthBeats)));
                const float endX = laneBounds.getX()
                    + (laneBounds.getWidth() * static_cast<float>(cellBeatEnd / juce::jmax(1.0, lengthBeats)));
                g.setColour(tint.withAlpha(0.16f));
                g.fillRoundedRectangle(juce::Rectangle<float>(startX,
                                                              laneBounds.getY() + 1.0f,
                                                              juce::jmax(2.0f, endX - startX),
                                                              juce::jmax(2.0f, laneBounds.getHeight() - 2.0f)),
                                       2.6f);
            }

            g.setColour(tint.withAlpha(sceneDrawModeEnabled ? 0.92f : 0.72f));
            g.drawLine(x, laneBounds.getY() + 1.0f, x, laneBounds.getBottom() - 1.0f, sceneDrawModeEnabled ? 1.8f : 1.2f);
        };

        const bool stepTriggerLane = layout.stepTriggerLane;
        auto* engine = focusedSceneIsActive ? processor.getAudioEngine() : nullptr;
        auto* strip = engine != nullptr
            ? engine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, visibleStrip))
            : nullptr;

        if (layout.compactOverview)
        {
            auto* overviewStrip = strip;
            if (overviewStrip == nullptr)
            {
                if (auto* overviewEngine = processor.getAudioEngine())
                    overviewStrip = overviewEngine->getStrip(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, visibleStrip));
            }

            g.setColour(kSurfaceDark.brighter(0.04f));
            g.fillRoundedRectangle(layout.triggerTimelineBounds, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawRoundedRectangle(layout.triggerTimelineBounds.reduced(0.5f), 4.0f, 1.0f);

            g.setColour(kTextMuted.withAlpha(0.92f));
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(stepTriggerLane ? "Steps" : "Wave",
                       layout.triggerLabelBounds.toNearestInt(),
                       juce::Justification::centredLeft,
                       false);

            auto overviewBounds = layout.triggerTimelineBounds.reduced(3.0f, 2.0f);
            drawBarBlocks(overviewBounds);

            if (stepTriggerLane)
            {
                const int totalSteps = juce::jmax(1, layout.stepTotalSteps);
                const int displayColumns = juce::jlimit(8, 16, totalSteps);
                const float cellGap = 1.0f;
                const float cellWidth = juce::jmax(2.0f,
                                                   (overviewBounds.getWidth() - (cellGap * static_cast<float>(displayColumns - 1)))
                                                       / static_cast<float>(displayColumns));
                const bool highlightPlayStep = overviewStrip != nullptr && overviewStrip->isPlaying();
                const int playStep = (overviewStrip != nullptr)
                    ? juce::jlimit(0, totalSteps - 1, overviewStrip->currentStep)
                    : -1;
                for (int column = 0; column < displayColumns; ++column)
                {
                    const int stepStart = (column * totalSteps) / displayColumns;
                    const int stepEnd = juce::jmax(stepStart + 1, ((column + 1) * totalSteps) / displayColumns);
                    bool enabled = false;
                    float probabilityPeak = 0.0f;
                    bool playStepInside = false;
                    if (overviewStrip != nullptr)
                    {
                        for (int step = stepStart; step < juce::jmin(totalSteps, stepEnd); ++step)
                        {
                            enabled = enabled || overviewStrip->stepPattern[static_cast<size_t>(step)];
                            probabilityPeak = juce::jmax(probabilityPeak,
                                                         juce::jlimit(0.0f,
                                                                      1.0f,
                                                                      overviewStrip->stepProbability[static_cast<size_t>(step)]));
                            playStepInside = playStepInside || (highlightPlayStep && step == playStep);
                        }
                    }

                    auto cellBounds = juce::Rectangle<float>(overviewBounds.getX() + (column * (cellWidth + cellGap)),
                                                             overviewBounds.getY(),
                                                             cellWidth,
                                                             overviewBounds.getHeight());
                    g.setColour(enabled
                                    ? stripColour.withAlpha(0.28f + (probabilityPeak * 0.32f))
                                    : juce::Colour(0xff171b1f).withAlpha(0.82f));
                    g.fillRoundedRectangle(cellBounds, 1.8f);
                    g.setColour(playStepInside
                                    ? kAccent.withAlpha(0.96f)
                                    : juce::Colours::white.withAlpha(enabled ? 0.11f : 0.06f));
                    g.drawRoundedRectangle(cellBounds.reduced(0.3f), 1.8f, playStepInside ? 1.0f : 0.7f);
                }
            }
            else
            {
                const float centerY = overviewBounds.getCentreY();
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.drawHorizontalLine(static_cast<int>(std::round(centerY)),
                                     overviewBounds.getX() + 1.0f,
                                     overviewBounds.getRight() - 1.0f);

                if (overviewStrip != nullptr && overviewStrip->hasAudio())
                {
                    if (const auto* buffer = overviewStrip->getAudioBuffer();
                        buffer != nullptr && buffer->getNumSamples() > 0)
                    {
                        const int totalSamples = buffer->getNumSamples();
                        int sampleStart = 0;
                        int sampleEnd = totalSamples;
                        const int loopStart = juce::jlimit(0, juce::jmax(0, totalSamples - 1), overviewStrip->getLoopStart());
                        const int loopEnd = juce::jlimit(loopStart + 1, totalSamples, overviewStrip->getLoopEnd());
                        if (loopEnd > loopStart + 32)
                        {
                            sampleStart = loopStart;
                            sampleEnd = loopEnd;
                        }

                        const int pixelColumns = juce::jmax(18, static_cast<int>(std::round(overviewBounds.getWidth())));
                        const float halfHeight = overviewBounds.getHeight() * 0.42f;
                        for (int pixel = 0; pixel < pixelColumns; ++pixel)
                        {
                            const int regionStart = sampleStart
                                + ((pixel * (sampleEnd - sampleStart)) / pixelColumns);
                            const int regionEnd = sampleStart
                                + (((pixel + 1) * (sampleEnd - sampleStart)) / pixelColumns);
                            const int sampleStride = juce::jmax(1, (regionEnd - regionStart) / 8);
                            float peak = 0.0f;
                            for (int sample = regionStart; sample < juce::jmax(regionStart + 1, regionEnd); sample += sampleStride)
                            {
                                for (int channel = 0; channel < buffer->getNumChannels(); ++channel)
                                    peak = juce::jmax(peak, std::abs(buffer->getSample(channel, sample)));
                            }

                            const float x = overviewBounds.getX()
                                + (overviewBounds.getWidth() * ((static_cast<float>(pixel) + 0.5f) / static_cast<float>(pixelColumns)));
                            const float yExtent = juce::jlimit(0.0f, halfHeight, peak * halfHeight);
                            g.setColour(stripColour.withAlpha(0.28f + (peak * 0.34f)));
                            g.drawLine(x,
                                       centerY - yExtent,
                                       x,
                                       centerY + yExtent,
                                       1.0f);
                        }
                    }
                }
            }

            for (const auto& event : sceneEditorState.events)
            {
                if (event.stripIndex != visibleStrip)
                    continue;

                const float x = overviewBounds.getX()
                    + (overviewBounds.getWidth()
                       * static_cast<float>(event.timeBeats / juce::jmax(1.0, lengthBeats)));
                if (event.type == ScenePerformanceEventType::Trigger)
                {
                    g.setColour(stripColour.withAlpha(event.isNoteOn ? 0.92f : 0.62f));
                    g.drawLine(x,
                               overviewBounds.getY() + 1.0f,
                               x,
                               overviewBounds.getBottom() - 1.0f,
                               1.15f);
                }
                else
                {
                    const float normalizedValue = normalizeSceneAutomationValue(event);
                    const float yPos = overviewBounds.getBottom() - 2.0f
                        - (juce::jlimit(0.0f, 1.0f, normalizedValue)
                           * juce::jmax(3.0f, overviewBounds.getHeight() - 4.0f));
                    g.setColour(stripColour.brighter(0.35f).withAlpha(0.72f));
                    g.fillEllipse(x - 1.1f, yPos - 1.1f, 2.2f, 2.2f);
                }
            }

            if (focusedSceneIsActive && overviewStrip != nullptr && overviewStrip->isPlaying())
            {
                const auto phaseBounds = overviewBounds.reduced(0.5f, 0.0f);
                const float phase = static_cast<float>(juce::jlimit(0.0, 1.0, overviewStrip->getLoopPhaseNormalized()));
                const float phaseX = phaseBounds.getX() + (phaseBounds.getWidth() * phase);
                g.setColour(stripColour.withAlpha(0.95f));
                g.drawLine(phaseX,
                           phaseBounds.getY(),
                           phaseX,
                           phaseBounds.getBottom(),
                           1.2f);
            }

            if (!scenePlaybackAvailable)
            {
                g.setColour(kTextPrimary.withAlpha(0.82f));
                g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                g.drawText("Scene playback unavailable",
                           layout.triggerTimelineBounds.toNearestInt().reduced(8, 1),
                           juce::Justification::centred,
                           false);
            }
            else if (!stripHasEvents)
            {
                g.setColour(kTextMuted.withAlpha(0.52f));
                g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                const auto openHintBounds = juce::Rectangle<float>(layout.triggerTimelineBounds.getRight() - 38.0f,
                                                                   layout.triggerTimelineBounds.getY(),
                                                                   34.0f,
                                                                   layout.triggerTimelineBounds.getHeight())
                    .toNearestInt();
                g.drawText("Open",
                           openHintBounds,
                           juce::Justification::centredRight,
                           false);
            }

            y += cardHeight + kSceneCardGap;
            continue;
        }

        if (stepTriggerLane)
        {
            drawBarBlocks(triggerTimeBounds);
            drawSubGrid(triggerTimeBounds);
            drawHoverOverlay(triggerTimeBounds,
                             stripColour,
                             sceneEditorState.hoverActive
                                 && sceneEditorState.hoverTriggerLane
                                 && sceneEditorState.hoverStripIndex == visibleStrip);

            if (!layout.stepPatternBounds.isEmpty())
            {
                g.setColour(juce::Colour(0xff24272c));
                g.fillRoundedRectangle(layout.stepPatternBounds, 3.5f);
                g.setColour(juce::Colours::white.withAlpha(0.06f));
                g.drawRoundedRectangle(layout.stepPatternBounds.reduced(0.5f), 3.5f, 1.0f);

                int highlightedColumn = -1;
                if (sceneEditorState.hoverEventIndex >= 0
                    && sceneEditorState.hoverEventIndex < static_cast<int>(sceneEditorState.events.size()))
                {
                    const auto& hoveredEvent = sceneEditorState.events[static_cast<size_t>(sceneEditorState.hoverEventIndex)];
                    if (hoveredEvent.type == ScenePerformanceEventType::Trigger && hoveredEvent.stripIndex == visibleStrip)
                        highlightedColumn = juce::jlimit(0, kSceneStepColumnsPerRow - 1, hoveredEvent.column);
                }
                if (highlightedColumn < 0
                    && sceneEditorState.selectedSceneSlot == sceneSlot
                    && sceneEditorState.selectedEventIndex >= 0
                    && sceneEditorState.selectedEventIndex < static_cast<int>(sceneEditorState.events.size()))
                {
                    const auto& selectedEvent = sceneEditorState.events[static_cast<size_t>(sceneEditorState.selectedEventIndex)];
                    if (selectedEvent.type == ScenePerformanceEventType::Trigger && selectedEvent.stripIndex == visibleStrip)
                        highlightedColumn = juce::jlimit(0, kSceneStepColumnsPerRow - 1, selectedEvent.column);
                }

                const int totalSteps = juce::jmax(1, layout.stepTotalSteps);
                const int playStep = strip != nullptr ? juce::jlimit(0, totalSteps - 1, strip->currentStep) : -1;
                const bool highlightPlayStep = strip != nullptr && strip->isPlaying();
                for (int step = 0; step < totalSteps; ++step)
                {
                    auto stepRect = sceneStepCellBounds(layout, step);
                    if (stepRect.isEmpty())
                        continue;

                    const bool enabled = strip != nullptr && strip->stepPattern[static_cast<size_t>(step)];
                    const float probability = strip != nullptr
                        ? juce::jlimit(0.0f, 1.0f, strip->stepProbability[static_cast<size_t>(step)])
                        : 0.0f;
                    const int subdivision = strip != nullptr
                        ? juce::jlimit(1, 16, strip->stepSubdivisions[static_cast<size_t>(step)])
                        : 1;
                    const bool highlightColumn = highlightedColumn == (step % kSceneStepColumnsPerRow);
                    const bool isPlayStep = highlightPlayStep && step == playStep;

                    g.setColour(highlightColumn
                                    ? stripColour.withAlpha(enabled ? 0.22f : 0.12f)
                                    : juce::Colour(0xff15171a).withAlpha(enabled ? 0.94f : 0.76f));
                    g.fillRoundedRectangle(stepRect, 2.4f);

                    if (enabled)
                    {
                        auto fillRect = stepRect.reduced(1.0f, 1.0f);
                        g.setColour(stripColour.withAlpha(0.76f));
                        g.fillRoundedRectangle(fillRect, 2.0f);

                        const float trackHeight = juce::jlimit(2.0f, 5.0f, fillRect.getHeight() * 0.16f);
                        auto probabilityTrack = juce::Rectangle<float>(fillRect.getX() + 1.0f,
                                                                       fillRect.getY() + 1.0f,
                                                                       juce::jmax(2.0f, fillRect.getWidth() - 2.0f),
                                                                       trackHeight);
                        g.setColour(stripColour.darker(1.8f).withAlpha(0.9f));
                        g.fillRoundedRectangle(probabilityTrack, 1.0f);
                        if (probability > 0.0f)
                        {
                            g.setColour(stripColour.brighter(0.35f).withAlpha(0.96f));
                            g.fillRoundedRectangle(probabilityTrack.withWidth(probabilityTrack.getWidth() * probability), 1.0f);
                        }
                        if (probability < 0.995f)
                        {
                            const float hatchX = probabilityTrack.getX() + (probabilityTrack.getWidth() * probability);
                            g.setColour(stripColour.withAlpha(0.34f));
                            for (float x = hatchX; x < probabilityTrack.getRight(); x += 4.0f)
                            {
                                g.drawLine(x,
                                           probabilityTrack.getY(),
                                           juce::jmin(x + 2.5f, probabilityTrack.getRight()),
                                           probabilityTrack.getBottom(),
                                           0.9f);
                            }
                        }
                    }

                    g.setColour(isPlayStep
                                    ? kAccent.withAlpha(0.96f)
                                    : (highlightColumn
                                           ? juce::Colours::white.withAlpha(0.4f)
                                           : juce::Colours::white.withAlpha(enabled ? 0.12f : 0.06f)));
                    g.drawRoundedRectangle(stepRect.reduced(0.35f), 2.4f, isPlayStep ? 1.35f : 0.8f);

                    g.setColour(enabled
                                    ? kTextPrimary.withAlpha(0.94f)
                                    : kTextMuted.withAlpha(0.5f));
                    g.setFont(juce::Font(juce::FontOptions(stepRect.getHeight() >= 24.0f ? 9.5f : 8.0f,
                                                           juce::Font::bold)));
                    g.drawText(juce::String(step + 1),
                               stepRect.toNearestInt().reduced(4, 2),
                               juce::Justification::centredLeft,
                               false);

                    if (subdivision > 1)
                    {
                        g.setColour(kTextMuted.withAlpha(enabled ? 0.88f : 0.55f));
                        g.setFont(juce::Font(juce::FontOptions(stepRect.getHeight() >= 24.0f ? 8.0f : 7.0f,
                                                               juce::Font::bold)));
                        g.drawText("x" + juce::String(subdivision),
                                   stepRect.toNearestInt().reduced(4, 2),
                                   juce::Justification::centredRight,
                                   false);
                    }
                }
            }
        }
        else
        {
            drawBarBlocks(layout.triggerTimelineBounds);
            drawSubGrid(layout.triggerTimelineBounds);
            drawHoverOverlay(layout.triggerTimelineBounds,
                             stripColour,
                             sceneEditorState.hoverActive
                                 && sceneEditorState.hoverTriggerLane
                                 && sceneEditorState.hoverStripIndex == visibleStrip);
        }

        const int wholeBeats = juce::jmax(1, static_cast<int>(std::ceil(lengthBeats)));
        for (int beatIndex = 0; beatIndex <= wholeBeats; ++beatIndex)
        {
            const float x = layout.rulerTimelineBounds.getX()
                + (layout.rulerTimelineBounds.getWidth() * static_cast<float>(beatIndex) / static_cast<float>(wholeBeats));
            const bool isBarStart = (beatIndex % 4) == 0;
            const float alpha = (beatIndex == 0 || beatIndex >= wholeBeats)
                ? 0.18f
                : (isBarStart ? 0.13f : 0.08f);
            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.drawVerticalLine(static_cast<int>(std::round(x)), timelineUnion.getY(), timelineUnion.getBottom());

            if (beatIndex < wholeBeats)
            {
                g.setColour(juce::Colours::white.withAlpha(isBarStart ? 0.26f : 0.16f));
                const float tickTop = isBarStart
                    ? layout.rulerTimelineBounds.getY() + 2.0f
                    : layout.rulerTimelineBounds.getY() + 6.0f;
                g.drawLine(x,
                           tickTop,
                           x,
                           layout.rulerTimelineBounds.getBottom() - 2.0f,
                           isBarStart ? 1.1f : 0.9f);

                const float beatWidth = layout.rulerTimelineBounds.getWidth() / static_cast<float>(wholeBeats);
                const bool labelEachBeat = beatWidth >= 34.0f;
                if (labelEachBeat || isBarStart)
                {
                    const int barNumber = (beatIndex / 4) + 1;
                    const int beatInBar = (beatIndex % 4) + 1;
                    const juce::String label = labelEachBeat
                        ? (juce::String(barNumber) + "." + juce::String(beatInBar))
                        : ("B" + juce::String(barNumber));
                    g.setColour(kTextMuted.withAlpha(isBarStart ? 0.95f : 0.72f));
                    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                    g.drawText(label,
                               juce::Rectangle<int>(static_cast<int>(std::round(x + 2.0f)),
                                                    static_cast<int>(layout.rulerTimelineBounds.getY()),
                                                    static_cast<int>(juce::jmax(20.0f, beatWidth - 4.0f)),
                                                    static_cast<int>(layout.rulerTimelineBounds.getHeight())),
                               juce::Justification::centredLeft,
                               false);
                }
            }
        }

        g.setColour(kTextMuted);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText("Bars/Beats", layout.rulerLabelBounds.toNearestInt(), juce::Justification::centredLeft);
        g.drawText(sceneTriggerLaneSummaryText(processor, visibleStrip, layout.heightExpanded, sceneLengthLabel),
                   layout.triggerLabelBounds.toNearestInt(),
                   juce::Justification::centredLeft);

        const bool triggerLaneHovered = sceneEditorState.hoverActive
            && sceneEditorState.hoverTriggerLane
            && sceneEditorState.hoverStripIndex == visibleStrip;
        if (!stepTriggerLane)
        {
            drawSceneHeaderActionChip(g,
                                      juce::Rectangle<float>(layout.triggerTimelineBounds.getX() + 24.0f,
                                                             layout.triggerTimelineBounds.getY() + 2.0f,
                                                             42.0f,
                                                             12.0f),
                                      "Offset",
                                      (triggerLaneHovered && sceneEditorState.hoverTriggerMoveOffset)
                                          ? kAccent
                                          : stripColour.withAlpha(0.8f),
                                      true);
            drawSceneHeaderActionChip(g,
                                      juce::Rectangle<float>(layout.triggerTimelineBounds.getX() + 24.0f,
                                                             layout.triggerTimelineBounds.getBottom() - 14.0f,
                                                             34.0f,
                                                             12.0f),
                                      "Time",
                                      (triggerLaneHovered && sceneEditorState.hoverTriggerMoveTime)
                                          ? kAccent
                                          : stripColour.withAlpha(0.68f),
                                      true);
        }

        if (!stepTriggerLane)
        {
            for (int guideColumn : {0, 4, 8, 12, 15})
            {
                const float normalizedGuide = juce::jlimit(0.0f, 1.0f, guideColumn / 15.0f);
                const float guideY = layout.triggerTimelineBounds.getBottom() - 4.0f
                    - (normalizedGuide * juce::jmax(4.0f, layout.triggerTimelineBounds.getHeight() - 8.0f));
                const float alpha = (guideColumn == 8) ? 0.12f : 0.06f;
                g.setColour(juce::Colours::white.withAlpha(alpha));
                g.drawHorizontalLine(static_cast<int>(std::round(guideY)),
                                     layout.triggerTimelineBounds.getX() + 1.0f,
                                     layout.triggerTimelineBounds.getRight() - 1.0f);
            }

            g.setFont(juce::Font(juce::FontOptions(7.6f, juce::Font::bold)));
            for (const auto& guide : std::array<std::pair<int, const char*>, 3>{{
                     {15, "16"}, {8, "9"}, {0, "1"}
                 }})
            {
                const float normalizedGuide = juce::jlimit(0.0f, 1.0f, guide.first / 15.0f);
                const float guideY = layout.triggerTimelineBounds.getBottom() - 4.0f
                    - (normalizedGuide * juce::jmax(4.0f, layout.triggerTimelineBounds.getHeight() - 8.0f));
                const auto labelBounds = juce::Rectangle<int>(static_cast<int>(layout.triggerTimelineBounds.getX() + 4.0f),
                                                              static_cast<int>(std::round(guideY - 6.0f)),
                                                              18,
                                                              12);
                g.setColour(kTextMuted.withAlpha(guide.first == 8 ? 0.92f : 0.72f));
                g.drawText(guide.second, labelBounds, juce::Justification::centredLeft, false);
            }
        }

        if (focusedSceneIsActive)
        {
            if (auto* liveEngine = processor.getAudioEngine())
            {
                if (auto* liveStrip = liveEngine->getStrip(visibleStrip); liveStrip != nullptr && liveStrip->isPlaying())
                {
                    const auto phaseBounds = layout.rulerTimelineBounds.reduced(2.0f, 2.0f);
                    const float phase = static_cast<float>(juce::jlimit(0.0, 1.0, liveStrip->getLoopPhaseNormalized()));
                    const float phaseX = phaseBounds.getX() + (phaseBounds.getWidth() * phase);
                    const float markerSize = juce::jmin(8.0f, phaseBounds.getHeight() - 1.0f);
                    const auto markerBounds = juce::Rectangle<float>(markerSize, markerSize)
                        .withCentre({ phaseX, phaseBounds.getCentreY() });
                    g.setColour(stripColour.withAlpha(0.97f));
                    g.fillRoundedRectangle(markerBounds, 1.6f);
                    g.setColour(juce::Colours::white.withAlpha(0.78f));
                    g.drawRoundedRectangle(markerBounds.reduced(0.4f), 1.4f, 1.0f);
                }
            }
        }

        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (event.type != ScenePerformanceEventType::Trigger || event.stripIndex != visibleStrip)
                continue;

            const bool isSelected = isSceneEventIndexSelected(eventIndex)
                && sceneEditorState.selectedSceneSlot == sceneSlot;
            const bool isHovered = sceneEditorState.hoverEventIndex == eventIndex
                && sceneEditorState.hoverTriggerLane
                && sceneEditorState.hoverStripIndex == visibleStrip;
            const bool hoverMoveTime = isHovered && sceneEditorState.hoverTriggerMoveTime;
            const bool hoverMoveOffset = isHovered && sceneEditorState.hoverTriggerMoveOffset;
            auto marker = sceneTriggerMarkerBounds(layout, event, lengthBeats);
            auto timeHandle = sceneTriggerTimeHandleBounds(layout, event, lengthBeats);
            const auto hoverColour = kAccent.withAlpha(0.95f);
            if (!stepTriggerLane)
            {
                g.setColour((isSelected || isHovered) ? stripColour.withAlpha(0.42f)
                                                      : stripColour.withAlpha(0.18f));
                g.drawLine(marker.getCentreX(),
                           layout.triggerTimelineBounds.getBottom() - 3.0f,
                           marker.getCentreX(),
                           marker.getCentreY(),
                           (isSelected || isHovered) ? 1.35f : 1.0f);
                g.setColour((isSelected || hoverMoveTime)
                                ? hoverColour
                                : stripColour.withAlpha(isHovered ? 0.72f : (isSelected ? 0.78f : 0.5f)));
                g.fillRoundedRectangle(timeHandle, 1.8f);
                g.setColour((isSelected || hoverMoveOffset)
                                ? hoverColour
                                : stripColour.withAlpha(event.isNoteOn ? 0.95f : 0.55f));
                g.fillRoundedRectangle(marker.reduced(0.9f, 0.8f), 2.2f);
                if (isSelected || hoverMoveOffset)
                {
                    g.setColour((isSelected && !hoverMoveOffset)
                                    ? juce::Colours::white.withAlpha(0.9f)
                                    : hoverColour.withAlpha(0.96f));
                    g.drawRoundedRectangle(marker.reduced(0.5f, 0.3f), 2.2f, 1.1f);
                }
                if (isSelected || hoverMoveTime)
                {
                    g.setColour((isSelected && !hoverMoveTime)
                                    ? juce::Colours::white.withAlpha(0.9f)
                                    : hoverColour.withAlpha(0.96f));
                    g.drawRoundedRectangle(timeHandle.reduced(0.4f, 0.2f), 1.8f, 1.0f);
                }
            }
            else
            {
                g.setColour((isSelected || hoverMoveTime)
                                ? hoverColour
                                : stripColour.withAlpha(event.isNoteOn ? 0.95f : 0.58f));
                g.fillRoundedRectangle(marker.reduced(0.8f, 0.6f), 3.2f);
                g.setColour((isSelected || isHovered)
                                ? juce::Colours::white.withAlpha(0.94f)
                                : stripColour.withAlpha(0.42f));
                g.drawRoundedRectangle(marker.reduced(0.45f, 0.35f), 3.2f, isSelected ? 1.2f : 0.9f);
            }

            g.setColour(kTextPrimary.withAlpha((isSelected || isHovered) ? 0.98f : 0.86f));
            g.setFont(juce::Font(juce::FontOptions(stepTriggerLane ? 8.5f : 8.0f, juce::Font::bold)));
            g.drawText(juce::String(juce::jlimit(0, 15, event.column) + 1),
                       marker.toNearestInt(),
                       juce::Justification::centred,
                       false);
        }

        if (scenePlaybackAvailable && triggerCount == 0)
        {
            g.setColour(kTextMuted.withAlpha(0.6f));
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText(stepTriggerLane
                           ? "Double-click the launch rail to add a step launch"
                           : "Double-click to add a trigger with offset",
                       (stepTriggerLane ? triggerTimeBounds : layout.triggerTimelineBounds).toNearestInt(),
                       juce::Justification::centred);
        }

        if (!scenePlaybackAvailable)
        {
            g.setColour(kSurfaceDark.brighter(0.05f).withAlpha(0.94f));
            g.fillRoundedRectangle(layout.triggerTimelineBounds.reduced(0.8f), 4.0f);
            g.setColour(stripColour.withAlpha(0.28f));
            g.drawRoundedRectangle(layout.triggerTimelineBounds.reduced(0.8f), 4.0f, 1.0f);
            g.setColour(kTextPrimary.withAlpha(0.92f));
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText(sceneStripPlaybackUnavailableReason(processor, visibleStrip),
                       layout.triggerTimelineBounds.toNearestInt().reduced(12, 10),
                       juce::Justification::centred,
                       true);
        }

        g.setColour(kTextMuted);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        const auto automationTitle = scenePlaybackAvailable
            ? (automationExpanded ? "Automation lanes  [-]" : "Automation lanes  [+]")
            : juce::String("Automation lanes unavailable");
        g.drawText(automationTitle, layout.automationToggleBounds.toNearestInt(), juce::Justification::centredLeft);

        if (!scenePlaybackAvailable)
        {
            g.setColour(kTextMuted.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText(stripHasEvents
                           ? "Existing scene events on this strip are ignored in scene mode"
                           : sceneStripPlaybackUnavailableReason(processor, visibleStrip),
                       layout.automationHeaderBounds.toNearestInt(),
                       juce::Justification::centredRight);
        }
        else if (!automationExpanded)
        {
            if (controlCount == 0)
            {
                g.setColour(kTextMuted.withAlpha(0.8f));
                g.setFont(juce::Font(juce::FontOptions(9.0f)));
                g.drawText("No automation recorded", layout.automationHeaderBounds.toNearestInt(), juce::Justification::centredRight);
            }
        }
        else
        {
            const bool showMotionTargetSelectors = shouldShowSceneMotionTargetSelectors(visibleStrip);
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                auto labelBounds = layout.automationLabelBounds[static_cast<size_t>(lane)];
                const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                if (laneBounds.isEmpty())
                    continue;
                const auto laneTint = sceneAutomationColour(
                    makeDefaultSceneControlEventForLane(visibleStrip,
                                                        lane,
                                                        0.0,
                                                        sceneDefaultNormalizedValueForLane(lane)));
                const bool bipolarLane = sceneAutomationLaneIsBipolar(lane);
                const bool hasTargetDropdown = showMotionTargetSelectors
                    && !layout.motionTargetBounds[static_cast<size_t>(lane)].isEmpty();
                const bool laneModSelected = laneIsModSelected(visibleStrip, lane);
                int laneEventCount = 0;
                for (const auto& event : sceneEditorState.events)
                {
                    if (event.type == ScenePerformanceEventType::ControlPoint
                        && event.stripIndex == visibleStrip
                        && sceneAutomationLaneIndex(event) == lane)
                    {
                        ++laneEventCount;
                    }
                }
                const auto laneTarget = sceneAutomationLaneTarget(lane);
                const bool laneOverrideActive = focusedSceneIsActive
                    && processor.isActiveSceneAutomationOverridden(visibleStrip, laneTarget);
                float liveBaseNormalized = sceneDefaultNormalizedValueForLane(lane);
                const bool hasLiveBaseValue = focusedSceneIsActive
                    && sceneCurrentBaseNormalizedValue(processor,
                                                       visibleStrip,
                                                       laneTarget,
                                                       liveBaseNormalized);
                if (hasLiveBaseValue)
                    liveBaseNormalized = juce::jlimit(0.0f, 1.0f, liveBaseNormalized);
                const bool showLiveHoldOverlay = hasLiveBaseValue && (laneEventCount == 0 || laneOverrideActive);
                auto nameBounds = labelBounds;
                if (hasTargetDropdown)
                    nameBounds = juce::Rectangle<float>(labelBounds.getX(),
                                                        labelBounds.getY(),
                                                        labelBounds.getWidth(),
                                                        juce::jmax(7.0f, labelBounds.getHeight() * 0.42f));
                auto laneSelectionBounds = labelBounds.getUnion(laneBounds).expanded(5.0f, 1.5f);
                if (hasTargetDropdown)
                    laneSelectionBounds = laneSelectionBounds.getUnion(
                        layout.motionTargetBounds[static_cast<size_t>(lane)]).expanded(2.0f, 0.0f);
                if (laneModSelected)
                {
                    g.setColour(laneTint.withAlpha(mainModSceneLaneSelectionActive ? 0.12f : 0.09f));
                    g.fillRoundedRectangle(laneSelectionBounds, 4.0f);
                    g.setColour(laneTint.withAlpha(mainModSceneLaneSelectionActive ? 0.30f : 0.22f));
                    g.drawRoundedRectangle(laneSelectionBounds.reduced(0.5f), 4.0f, 1.0f);
                    g.setColour(laneTint.withAlpha(mainModSceneLaneSelectionActive ? 0.88f : 0.72f));
                    g.fillRoundedRectangle(juce::Rectangle<float>(laneSelectionBounds.getX() + 1.0f,
                                                                  laneSelectionBounds.getY() + 2.0f,
                                                                  2.5f,
                                                                  juce::jmax(4.0f, laneSelectionBounds.getHeight() - 4.0f)),
                                           1.25f);
                }
                g.setColour(kTextMuted);
                g.setFont(juce::Font(juce::FontOptions(8.8f, juce::Font::bold)));
                g.drawText(sceneAutomationLaneName(lane), nameBounds.toNearestInt(), juce::Justification::centredLeft);
                if (laneOverrideActive)
                {
                    drawSceneHeaderActionChip(g,
                                              juce::Rectangle<float>(laneBounds.getRight() - 50.0f,
                                                                     laneBounds.getY() + 2.0f,
                                                                     44.0f,
                                                                     12.0f),
                                              "Manual",
                                              kAccent,
                                              true);
                }

                g.setColour(kSurfaceDark.brighter(0.03f));
                g.fillRoundedRectangle(laneBounds, 3.0f);
                if (bipolarLane)
                {
                    const auto upperHalf = juce::Rectangle<float>(laneBounds.getX(),
                                                                  laneBounds.getY(),
                                                                  laneBounds.getWidth(),
                                                                  laneBounds.getHeight() * 0.5f);
                    const auto lowerHalf = juce::Rectangle<float>(laneBounds.getX(),
                                                                  laneBounds.getCentreY(),
                                                                  laneBounds.getWidth(),
                                                                  laneBounds.getHeight() * 0.5f);
                    g.setColour(laneTint.withAlpha(0.06f));
                    g.fillRoundedRectangle(upperHalf.reduced(0.8f, 0.6f), 2.6f);
                    g.setColour(laneTint.withAlpha(0.03f));
                    g.fillRoundedRectangle(lowerHalf.reduced(0.8f, 0.6f), 2.6f);
                    g.setColour(juce::Colours::white.withAlpha(0.14f));
                    g.drawHorizontalLine(static_cast<int>(std::round(laneBounds.getCentreY())),
                                         laneBounds.getX() + 2.0f,
                                         laneBounds.getRight() - 2.0f);
                    const auto scaleLabels = sceneAutomationLaneScaleLabels(lane);
                    g.setFont(juce::Font(juce::FontOptions(7.3f, juce::Font::bold)));
                    g.setColour(laneTint.withAlpha(0.88f));
                    g.drawText(scaleLabels[0],
                               juce::Rectangle<int>(static_cast<int>(laneBounds.getX() + 4.0f),
                                                    static_cast<int>(laneBounds.getY() + 1.0f),
                                                    28,
                                                    9),
                               juce::Justification::centredLeft,
                               false);
                    g.drawText(scaleLabels[1],
                               juce::Rectangle<int>(static_cast<int>(laneBounds.getX() + 4.0f),
                                                    static_cast<int>(std::round(laneBounds.getCentreY() - 5.0f)),
                                                    28,
                                                    10),
                               juce::Justification::centredLeft,
                               false);
                    g.drawText(scaleLabels[2],
                               juce::Rectangle<int>(static_cast<int>(laneBounds.getX() + 4.0f),
                                                    static_cast<int>(laneBounds.getBottom() - 10.0f),
                                                    28,
                                                    9),
                               juce::Justification::centredLeft,
                               false);
                }
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.drawRoundedRectangle(laneBounds.reduced(0.5f), 3.0f, 1.0f);
                drawHoverOverlay(laneBounds,
                                 laneTint,
                                 sceneEditorState.hoverActive
                                     && !sceneEditorState.hoverTriggerLane
                                     && sceneEditorState.hoverStripIndex == visibleStrip
                                     && sceneEditorState.hoverLaneIndex == lane);
                drawBarBlocks(laneBounds);
                drawSubGrid(laneBounds);
                drawSceneModLanePreview(g,
                                        processor,
                                        visibleStrip,
                                        lane,
                                        laneBounds,
                                        lengthBeats,
                                        laneTint);
                drawSceneLaneModControls(g,
                                         processor,
                                         visibleStrip,
                                         lane,
                                         laneBounds,
                                         juce::Colour(0xffd9a04a));

                std::array<juce::Point<float>,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> previousPoints{};
                std::array<juce::Point<float>,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> firstPoints{};
                std::array<juce::Point<float>,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> lastPoints{};
                std::array<juce::Colour,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> connectionColours{};
                std::array<bool,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> steppedConnections{};
                std::array<bool,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> firstSteppedConnections{};
                std::array<bool,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> hasPrevious{};
                std::array<bool,
                           static_cast<size_t>(ScenePerformanceControlTarget::GrainShape) + 1> hasFirst{};

                for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
                {
                    const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                    if (event.type != ScenePerformanceEventType::ControlPoint
                        || event.stripIndex != visibleStrip
                        || sceneAutomationLaneIndex(event) != lane)
                    {
                        continue;
                    }

                    auto marker = sceneControlMarkerBounds(layout, event, lengthBeats);
                    const auto point = marker.getCentre();
                    const auto targetIndex = static_cast<size_t>(juce::jlimit(
                        0,
                        static_cast<int>(ScenePerformanceControlTarget::GrainShape),
                        static_cast<int>(event.controlTarget)));
                    const auto connectionColour = laneOverrideActive
                        ? kTextMuted.withAlpha(0.28f)
                        : sceneAutomationColour(event).withAlpha(0.4f);
                    const bool steppedConnection = sceneAutomationEventUsesSteppedSegments(event);
                    if (!hasFirst[targetIndex])
                    {
                        firstPoints[targetIndex] = point;
                        firstSteppedConnections[targetIndex] = steppedConnection;
                        hasFirst[targetIndex] = true;
                    }
                    if (hasPrevious[targetIndex])
                    {
                        drawSceneAutomationConnection(g,
                                                      previousPoints[targetIndex],
                                                      point,
                                                      connectionColour,
                                                      steppedConnection,
                                                      1.2f);
                    }
                    previousPoints[targetIndex] = point;
                    lastPoints[targetIndex] = point;
                    connectionColours[targetIndex] = connectionColour;
                    steppedConnections[targetIndex] = steppedConnection;
                    hasPrevious[targetIndex] = true;
                }

                for (size_t targetIndex = 0; targetIndex < hasFirst.size(); ++targetIndex)
                {
                    if (!hasFirst[targetIndex])
                        continue;

                    drawSceneAutomationConnection(g,
                                                  { laneBounds.getX(), firstPoints[targetIndex].y },
                                                  firstPoints[targetIndex],
                                                  connectionColours[targetIndex],
                                                  firstSteppedConnections[targetIndex],
                                                  1.2f);
                    drawSceneAutomationConnection(g,
                                                  lastPoints[targetIndex],
                                                  { laneBounds.getRight(), lastPoints[targetIndex].y },
                                                  connectionColours[targetIndex],
                                                  steppedConnections[targetIndex],
                                                  1.2f);
                }

                for (int eventIndex = 0; eventIndex < static_cast<int>(sceneEditorState.events.size()); ++eventIndex)
                {
                    const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                    if (event.type != ScenePerformanceEventType::ControlPoint
                        || event.stripIndex != visibleStrip
                        || sceneAutomationLaneIndex(event) != lane)
                    {
                        continue;
                    }

                    const bool isSelected = isSceneEventIndexSelected(eventIndex)
                        && sceneEditorState.selectedSceneSlot == sceneSlot;
                    drawSceneAutomationPoint(g,
                                             event,
                                             sceneControlMarkerBounds(layout, event, lengthBeats),
                                             isSelected,
                                             laneOverrideActive
                                                 ? kTextMuted.withAlpha(isSelected ? 0.72f : 0.46f)
                                                 : juce::Colour());
                }

                if (showLiveHoldOverlay)
                {
                    const auto startEvent = makeDefaultSceneControlEventForLane(visibleStrip,
                                                                               lane,
                                                                               0.0,
                                                                               liveBaseNormalized);
                    const auto endEvent = makeDefaultSceneControlEventForLane(visibleStrip,
                                                                             lane,
                                                                             sceneAutomationWriteEndBeat(lengthBeats),
                                                                             liveBaseNormalized);
                    const auto startMarker = sceneControlMarkerBounds(layout, startEvent, lengthBeats);
                    const auto endMarker = sceneControlMarkerBounds(layout, endEvent, lengthBeats);
                    const auto liveColour = laneOverrideActive
                        ? kTextMuted.withAlpha(0.9f)
                        : laneTint.withAlpha(0.82f);

                    g.setColour(liveColour.withAlpha(laneOverrideActive ? 0.82f : 0.72f));
                    g.drawLine(startMarker.getCentreX(),
                               startMarker.getCentreY(),
                               endMarker.getCentreX(),
                               endMarker.getCentreY(),
                               laneOverrideActive ? 1.8f : 1.35f);

                    const float anchorSize = juce::jmax(2.4f, startMarker.getWidth() * 0.58f);
                    const auto startAnchorBounds = juce::Rectangle<float>(anchorSize, anchorSize)
                        .withCentre(startMarker.getCentre());
                    const auto endAnchorBounds = juce::Rectangle<float>(anchorSize, anchorSize)
                        .withCentre(endMarker.getCentre());
                    g.setColour(liveColour);
                    g.fillEllipse(startAnchorBounds);
                    g.fillEllipse(endAnchorBounds);
                    g.setColour(juce::Colours::white.withAlpha(laneOverrideActive ? 0.72f : 0.48f));
                    g.drawEllipse(startAnchorBounds.reduced(-0.35f), 0.85f);
                    g.drawEllipse(endAnchorBounds.reduced(-0.35f), 0.85f);
                }

                if (sceneEditorState.transportProgress >= 0.0f)
                {
                    const auto overlapTargets = sceneOverlappingTargetsForLane(processor,
                                                                              sceneEditorState.events,
                                                                              visibleStrip,
                                                                              lane);
                    if (!overlapTargets.empty())
                    {
                        const double playheadBeat = juce::jlimit(
                            0.0,
                            std::nextafter(lengthBeats, 0.0),
                            static_cast<double>(sceneEditorState.transportProgress) * lengthBeats);
                        const float headX = laneBounds.getX()
                            + (laneBounds.getWidth() * sceneEditorState.transportProgress);
                        const float normHeight = juce::jmax(4.0f, laneBounds.getHeight() - 4.0f);
                        const int overlayCount = juce::jmin(static_cast<int>(overlapTargets.size()), 3);
                        for (int overlayIndex = 0; overlayIndex < overlayCount; ++overlayIndex)
                        {
                            const auto target = overlapTargets[static_cast<size_t>(overlayIndex)];
                            float heldNorm = 0.0f;
                            float effectiveNorm = 0.0f;
                            if (!sceneHeldNormalizedValueForTarget(sceneEditorState.events,
                                                                   visibleStrip,
                                                                   target,
                                                                   playheadBeat,
                                                                   lengthBeats,
                                                                   heldNorm)
                                || !sceneCurrentEffectiveNormalizedValue(processor,
                                                                        visibleStrip,
                                                                        target,
                                                                        effectiveNorm))
                            {
                                continue;
                            }

                            const float xOffset = (static_cast<float>(overlayIndex)
                                                   - ((static_cast<float>(overlayCount) - 1.0f) * 0.5f))
                                * 7.0f;
                            const float overlayX = juce::jlimit(laneBounds.getX() + 10.0f,
                                                                 laneBounds.getRight() - 10.0f,
                                                                 headX + xOffset);
                            const float heldY = laneBounds.getBottom() - (heldNorm * normHeight) - 2.0f;
                            const float effectiveY = laneBounds.getBottom() - (effectiveNorm * normHeight) - 2.0f;
                            ScenePerformanceEvent probeEvent;
                            probeEvent.type = ScenePerformanceEventType::ControlPoint;
                            probeEvent.controlTarget = target;
                            const auto targetColour = sceneAutomationColour(probeEvent);
                            const auto overlayColour = juce::Colour(0xffd9a04a);
                            const bool targetOverrideActive = focusedSceneIsActive
                                && processor.isActiveSceneAutomationOverridden(visibleStrip, target);
                            const auto heldColour = targetOverrideActive
                                ? kTextMuted.withAlpha(0.88f)
                                : targetColour.withAlpha(0.92f);
                            const auto connectorColour = targetOverrideActive
                                ? kTextMuted.withAlpha(0.34f)
                                : overlayColour.withAlpha(0.45f);
                            const auto labelColour = targetOverrideActive
                                ? kTextMuted.withAlpha(0.9f)
                                : overlayColour.withAlpha(0.92f);

                            g.setColour(connectorColour);
                            g.drawLine(overlayX, heldY, overlayX, effectiveY, 1.1f);
                            g.setColour(heldColour);
                            g.drawEllipse(overlayX - 3.2f, heldY - 3.2f, 6.4f, 6.4f, 1.15f);
                            g.setColour(overlayColour.withAlpha(0.96f));
                            g.fillEllipse(overlayX - 2.8f, effectiveY - 2.8f, 5.6f, 5.6f);

                            if (overlayCount > 1 || layout.heightExpanded)
                            {
                                const auto overlayLabelBounds = juce::Rectangle<float>(overlayX - 12.0f,
                                                                                       laneBounds.getY() + 2.0f + (overlayIndex * 10.0f),
                                                                                       24.0f,
                                                                                       9.0f);
                                g.setColour(labelColour);
                                g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                                g.drawText(sceneControlTargetShortName(target),
                                           overlayLabelBounds.toNearestInt(),
                                           juce::Justification::centred,
                                           false);
                            }
                        }
                    }
                }

                if (laneEventCount == 0 && !showLiveHoldOverlay)
                {
                    g.setColour(kTextMuted.withAlpha(0.42f));
                    g.setFont(juce::Font(juce::FontOptions(8.4f)));
                    g.drawText("Double-click to add " + sceneAutomationLaneCreateHint(lane),
                               laneBounds.toNearestInt(),
                               juce::Justification::centredRight);
                }
            }
        }

        if (sceneEditorState.transportProgress >= 0.0f)
        {
            const auto headBounds = stepTriggerLane ? triggerTimeBounds : layout.triggerTimelineBounds;
            const float headX = headBounds.getX()
                + (headBounds.getWidth() * sceneEditorState.transportProgress);
            const auto headColour = (sceneEditorState.transportRecording ? juce::Colour(0xffd46b62)
                                                                         : juce::Colour(0xff76be7e)).withAlpha(0.95f);
            g.setColour(headColour);
            g.drawLine(headX, timelineUnion.getY(), headX, timelineUnion.getBottom(), 1.4f);
        }

        y += cardHeight + kSceneCardGap;
    }

    if (sceneEditorState.marqueeActive && !sceneEditorState.marqueeRect.isEmpty())
    {
        g.setColour(kAccent.withAlpha(0.14f));
        g.fillRect(sceneEditorState.marqueeRect);
        g.setColour(kAccent.withAlpha(0.85f));
        g.drawRect(sceneEditorState.marqueeRect, 1.2f);
    }
}

void SceneControlPanel::handleSceneTimelineMouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    updateSceneHoverState(e.position, true);

    const int sceneSlot = getFocusedSceneSlot();
    const bool focusedSceneIsActive = sceneSlot == processor.getActiveSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const bool laneActionModifier = e.mods.isAltDown();
    const bool wantsThin = laneActionModifier && e.mods.isShiftDown();
    const bool wantsErase = e.mods.isPopupMenu()
        || e.mods.isRightButtonDown();
    const bool wantsDefaultLane = (e.mods.isCommandDown() || e.mods.isCtrlDown())
        && !e.mods.isShiftDown()
        && !e.mods.isAltDown()
        && !wantsErase;
    const bool wantsRetargetLaunch = (e.mods.isCommandDown() || e.mods.isCtrlDown()) && !e.mods.isShiftDown();
    const bool wantsToggleSelection = e.mods.isShiftDown()
        || e.mods.isCommandDown()
        || e.mods.isCtrlDown();
    const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
        0.0f,
        0.0f,
        static_cast<float>(sceneTimelineCanvas.getWidth()),
        sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
        sceneGlobalLaneExpanded);
    const int globalLane = sceneGlobalAutomationLaneIndex();
    const bool hadClickLineAnchor = sceneEditorState.clickLinePending;
    const int previousClickLineStripIndex = sceneEditorState.clickLineStripIndex;
    const int previousClickLineLaneIndex = sceneEditorState.clickLineLaneIndex;
    const double previousClickLineBeat = sceneEditorState.clickLineBeat;
    const float previousClickLineValue = sceneEditorState.clickLineValue;
    sceneEditorState.clickLinePending = false;
    sceneEditorState.clickLineStripIndex = -1;
    sceneEditorState.clickLineLaneIndex = -1;
    sceneEditorState.clickLineBeat = 0.0;
    sceneEditorState.clickLineValue = 0.0f;

    if (wantsDefaultLane
        && globalLane >= 0
        && globalLayout.laneBounds.contains(e.position))
    {
        defaultSceneLane(-1, globalLane);
        return;
    }

    if (laneActionModifier
        && globalLane >= 0
        && globalLayout.laneLabelBounds.expanded(4.0f, 2.0f).contains(e.position))
    {
        (wantsThin ? thinSceneLane(-1, globalLane, false) : clearSceneLane(-1, globalLane, false));
        return;
    }

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (cardBounds.expanded(0.0f, 2.0f).contains(e.position))
            sceneEditorState.selectedStripIndex = visibleStrip;
        if (layout.scenePlaybackAvailable && layout.stripWriteBounds.contains(e.position))
        {
            writeCurrentStripAutomation(visibleStrip);
            return;
        }

        if (layout.scenePlaybackAvailable && layout.stripWriteAllBounds.contains(e.position))
        {
            writeAllStripsAutomation();
            return;
        }

        if (layout.stripClearBounds.contains(e.position))
        {
            clearStripSceneEvents(visibleStrip);
            return;
        }

        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        if (layout.stripDuplicateBounds.contains(e.position))
        {
            duplicateStripSceneEventsToNext(visibleStrip);
            return;
        }

        if (layout.stripCopyBounds.contains(e.position))
        {
            juce::PopupMenu menu;
            for (int targetStrip = 0; targetStrip < getVisibleSceneStripCount(); ++targetStrip)
            {
                if (targetStrip == visibleStrip)
                    continue;
                if (!processor.isStripScenePlaybackAvailable(targetStrip))
                    continue;
                menu.addItem(targetStrip + 1, "Copy to Strip " + juce::String(targetStrip + 1));
            }

            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sceneTimelineCanvas),
                               [this, visibleStrip](int result)
                               {
                                   if (result > 0)
                                       copyStripSceneEvents(visibleStrip, result - 1);
                               });
            return;
        }

        const auto automationHeaderHitBounds = layout.automationHeaderBounds.getUnion(layout.automationToggleBounds).expanded(6.0f, 3.0f);
        if (automationHeaderHitBounds.contains(e.position))
        {
            stripAutomationExpanded[static_cast<size_t>(visibleStrip)]
                = !stripAutomationExpanded[static_cast<size_t>(visibleStrip)];
            processor.setSceneEditorStripAutomationExpanded(
                visibleStrip,
                stripAutomationExpanded[static_cast<size_t>(visibleStrip)]);
            updateSceneTimelineContentSize();
            sceneTimelineCanvas.repaint();
            return;
        }

        if (layout.compactOverview)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        if (wantsDefaultLane && layout.automationExpanded)
        {
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                if (!laneBounds.contains(e.position))
                    continue;

                defaultSceneLane(visibleStrip, lane);
                return;
            }
        }

        if (laneActionModifier && layout.triggerLabelBounds.expanded(4.0f, 2.0f).contains(e.position))
        {
            (wantsThin ? thinSceneLane(visibleStrip, -1, true) : clearSceneLane(visibleStrip, -1, true));
            return;
        }

        if (laneActionModifier && layout.automationExpanded)
        {
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                if (!layout.automationLabelBounds[static_cast<size_t>(lane)].expanded(4.0f, 2.0f).contains(e.position))
                    continue;

                (wantsThin ? thinSceneLane(visibleStrip, lane, false) : clearSceneLane(visibleStrip, lane, false));
                return;
            }
        }

        if (layout.automationExpanded)
        {
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                if (laneBounds.isEmpty())
                    continue;

                const auto controlHit = hitSceneLaneModControl(processor,
                                                               visibleStrip,
                                                               lane,
                                                               laneBounds,
                                                               e.position);
                if (controlHit != SceneLaneModControl::None)
                {
                    if (controlHit == SceneLaneModControl::Edit)
                        openLegacyModEditorForLane(visibleStrip, lane);
                    else
                        cycleSceneLaneModControl(processor, visibleStrip, lane, controlHit);

                    refreshFromProcessor();
                    sceneTimelineCanvas.repaint();
                    return;
                }
            }
        }

        if (!wantsErase
            && layout.stepTriggerLane
            && layout.stepPatternBounds.contains(e.position))
        {
            const int absoluteStep = sceneStepAbsoluteStepAtPosition(layout, e.position);
            if (absoluteStep >= 0)
            {
                if (wantsRetargetLaunch
                    && assignSelectedStepTriggerColumn(visibleStrip, absoluteStep % kSceneStepColumnsPerRow))
                {
                    refreshFromProcessor();
                    return;
                }

                auto* engine = focusedSceneIsActive ? processor.getAudioEngine() : nullptr;
                auto* strip = engine != nullptr ? engine->getStrip(visibleStrip) : nullptr;
                if (!wantsToggleSelection && strip != nullptr)
                {
                    const bool enabled = strip->stepPattern[static_cast<size_t>(absoluteStep)];
                    const bool nextEnabled = !enabled;
                    strip->setStepEnabledAtIndex(absoluteStep, nextEnabled, true);
                    sceneEditorState.stepPatternPaintActive = true;
                    sceneEditorState.stepPatternPaintStripIndex = visibleStrip;
                    sceneEditorState.stepPatternPaintLastStep = absoluteStep;
                    sceneEditorState.stepPatternPaintEnabled = nextEnabled;
                    refreshFromProcessor();
                    sceneTimelineCanvas.repaint();
                    return;
                }
            }
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    if (e.mods.isAltDown() && e.mods.isLeftButtonDown() && !processor.isScenePerformanceRecording())
    {
        if (globalLane >= 0)
        {
            for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
            {
                const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                if (!sceneIsGlobalAutomationEvent(event))
                    continue;

                const auto markerBounds = sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats).expanded(3.0f, 3.0f);
                if (!markerBounds.contains(e.position))
                    continue;

                auto editedEvents = sceneEditorState.events;
                auto& editedEvent = editedEvents[static_cast<size_t>(eventIndex)];
                const float defaultNormalizedValue = sceneDefaultNormalizedValueForLane(globalLane);
                editedEvent.value = denormalizeSceneAutomationValue(editedEvent, defaultNormalizedValue);
                editedEvent.column = juce::jlimit(0,
                                                  15,
                                                  static_cast<int>(std::round(defaultNormalizedValue * 15.0f)));
                const auto preferredEvent = editedEvent;
                applyEditedSceneEvents(std::move(editedEvents), eventIndex, &preferredEvent);
                return;
            }
        }

        y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }
            if (!layout.automationExpanded)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }

            for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
            {
                const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
                if (event.type != ScenePerformanceEventType::ControlPoint || event.stripIndex != visibleStrip)
                    continue;

                const int laneIndex = sceneAutomationLaneIndex(event);
                if (laneIndex < 0 || laneIndex >= kSceneAutomationLaneCount)
                    continue;

                const auto markerBounds = sceneControlMarkerBounds(layout, event, lengthBeats).expanded(3.0f, 3.0f);
                if (!markerBounds.contains(e.position))
                    continue;

                auto editedEvents = sceneEditorState.events;
                auto& editedEvent = editedEvents[static_cast<size_t>(eventIndex)];
                const float defaultNormalizedValue = sceneAutomationLaneIsBipolar(laneIndex)
                    ? 0.5f
                    : sceneDefaultNormalizedValueForLane(laneIndex);
                editedEvent.value = denormalizeSceneAutomationValue(editedEvent, defaultNormalizedValue);
                editedEvent.column = juce::jlimit(0,
                                                  15,
                                                  static_cast<int>(std::round(defaultNormalizedValue * 15.0f)));
                const auto preferredEvent = editedEvent;
                applyEditedSceneEvents(std::move(editedEvents), eventIndex, &preferredEvent);
                return;
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    if (wantsErase && !processor.isScenePerformanceRecording())
    {
        if (globalLane >= 0 && globalLayout.laneBounds.contains(e.position))
        {
            sceneEditorState.eraseActive = true;
            sceneEditorState.eraseTriggerLane = false;
            sceneEditorState.eraseStripIndex = -1;
            sceneEditorState.eraseLaneIndex = globalLane;
            eraseSceneLaneEventAt(-1,
                                  globalLane,
                                  false,
                                  sceneTimeBeatsForX(globalLayout.laneBounds, e.position.x, lengthBeats));
            return;
        }

        y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }
            if (layout.compactOverview)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }

            const auto triggerInteractionBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
                ? layout.stepLaunchBounds
                : layout.triggerTimelineBounds;
            if (triggerInteractionBounds.contains(e.position))
            {
                sceneEditorState.eraseActive = true;
                sceneEditorState.eraseTriggerLane = true;
                sceneEditorState.eraseStripIndex = visibleStrip;
                sceneEditorState.eraseLaneIndex = -1;
                eraseSceneLaneEventAt(visibleStrip,
                                      -1,
                                      true,
                                      sceneTimeBeatsForX(triggerInteractionBounds, e.position.x, lengthBeats));
                return;
            }

            if (layout.automationExpanded)
            {
                for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
                {
                    const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                    if (!laneBounds.contains(e.position))
                        continue;

                    sceneEditorState.eraseActive = true;
                    sceneEditorState.eraseTriggerLane = false;
                    sceneEditorState.eraseStripIndex = visibleStrip;
                    sceneEditorState.eraseLaneIndex = lane;
                    eraseSceneLaneEventAt(visibleStrip,
                                          lane,
                                          false,
                                          sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats));
                    return;
                }
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    if (e.mods.isAltDown() && e.mods.isLeftButtonDown() && !processor.isScenePerformanceRecording())
    {
        y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }

            if (layout.automationExpanded)
            {
                for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
                {
                    const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                    if (!laneBounds.contains(e.position) || !sceneAutomationLaneIsBipolar(lane))
                        continue;

                    applySceneDrawPoint(visibleStrip,
                                        lane,
                                        sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats),
                                        0.5f);
                    return;
                }
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    if (sceneDrawModeEnabled && !processor.isScenePerformanceRecording())
    {
        if (globalLane >= 0 && globalLayout.laneBounds.contains(e.position))
        {
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - globalLayout.laneBounds.getY())
                                                              / juce::jmax(1.0f, globalLayout.laneBounds.getHeight()));
            sceneEditorState.drawActive = true;
            sceneEditorState.drawTriggerLane = false;
            sceneEditorState.drawStripIndex = -1;
            sceneEditorState.drawLaneIndex = globalLane;
            sceneEditorState.drawLastBeat = sceneTimeBeatsForX(globalLayout.laneBounds, e.position.x, lengthBeats);
            sceneEditorState.drawLastValue = normalizedY;
            sceneEditorState.drawHasLastPoint = true;
            applySceneDrawCurveSegment(-1,
                                       globalLane,
                                       sceneEditorState.drawLastBeat,
                                       sceneEditorState.drawLastValue,
                                       sceneEditorState.drawLastBeat,
                                       sceneEditorState.drawLastValue);
            return;
        }

        y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }
            if (layout.compactOverview)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }

            const auto triggerInteractionBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
                ? layout.stepLaunchBounds
                : layout.triggerTimelineBounds;
            if (triggerInteractionBounds.contains(e.position))
            {
                sceneEditorState.drawActive = true;
                sceneEditorState.drawTriggerLane = true;
                sceneEditorState.drawStripIndex = visibleStrip;
                sceneEditorState.drawLaneIndex = -1;
                sceneEditorState.drawHasLastPoint = false;
                applySceneDrawTrigger(visibleStrip,
                                      sceneTimeBeatsForX(triggerInteractionBounds, e.position.x, lengthBeats),
                                      layout.stepTriggerLane
                                          ? defaultStepTriggerColumnForStrip(visibleStrip)
                                          : sceneTriggerColumnForY(layout.triggerTimelineBounds, e.position.y));
                return;
            }

            if (layout.automationExpanded)
            {
                for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
                {
                    const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                    if (!laneBounds.contains(e.position))
                        continue;

                    const float normalizedY = (e.mods.isAltDown() && sceneAutomationLaneIsBipolar(lane))
                        ? 0.5f
                        : (1.0f - juce::jlimit(0.0f,
                                               1.0f,
                                               (e.position.y - laneBounds.getY()) / juce::jmax(1.0f, laneBounds.getHeight())));
                    sceneEditorState.drawActive = true;
                    sceneEditorState.drawTriggerLane = false;
                    sceneEditorState.drawStripIndex = visibleStrip;
                    sceneEditorState.drawLaneIndex = lane;
                    sceneEditorState.drawLastBeat = sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats);
                    sceneEditorState.drawLastValue = normalizedY;
                    sceneEditorState.drawHasLastPoint = true;
                    applySceneDrawCurveSegment(visibleStrip,
                                               lane,
                                               sceneEditorState.drawLastBeat,
                                               sceneEditorState.drawLastValue,
                                               sceneEditorState.drawLastBeat,
                                               sceneEditorState.drawLastValue);
                    return;
                }
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    int hitIndex = -1;
    bool hitTriggerMoveTime = true;
    bool hitTriggerMoveOffset = true;
    if (globalLane >= 0)
    {
        for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            const auto marker = sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats);
            if (marker.expanded(3.0f, 3.0f).contains(e.position))
            {
                hitIndex = eventIndex;
                break;
            }
        }
    }

    y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }
        if (layout.compactOverview)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (event.stripIndex != visibleStrip)
                continue;

            juce::Rectangle<float> marker;
            if (event.type == ScenePerformanceEventType::Trigger)
            {
                marker = sceneTriggerInteractiveBounds(layout, event, lengthBeats);
                sceneResolveTriggerDragIntent(layout,
                                              event,
                                              lengthBeats,
                                              e.position,
                                              hitTriggerMoveTime,
                                              hitTriggerMoveOffset);
            }
            else
            {
                if (!layout.automationExpanded)
                    continue;
                marker = sceneControlMarkerBounds(layout, event, lengthBeats);
            }

            if (marker.expanded(3.0f, 3.0f).contains(e.position))
            {
                hitIndex = eventIndex;
                break;
            }
        }

        if (hitIndex >= 0)
            break;

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    if (hitIndex < 0
        && !sceneDrawModeEnabled
        && !processor.isScenePerformanceRecording()
        && !wantsToggleSelection
        && !wantsErase
        && !laneActionModifier
        && !wantsRetargetLaunch
        && e.getNumberOfClicks() == 1)
    {
        auto placeAutomationClickLine = [this,
                                         &e,
                                         lengthBeats,
                                         hadClickLineAnchor,
                                         previousClickLineStripIndex,
                                         previousClickLineLaneIndex,
                                         previousClickLineBeat,
                                         previousClickLineValue](int stripIndex,
                                                                 int laneIndex,
                                                                 juce::Rectangle<float> laneBounds)
        {
            const int resolvedStripIndex = sceneResolveAutomationStripIndex(stripIndex, laneIndex);
            const double clickedBeat = sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats);
            const double snappedBeat = snapSceneBeatToGrid(clickedBeat, lengthBeats);
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - laneBounds.getY())
                                                              / juce::jmax(1.0f, laneBounds.getHeight()));
            const bool extendPreviousLine = hadClickLineAnchor
                && previousClickLineLaneIndex == laneIndex
                && previousClickLineStripIndex == resolvedStripIndex;

            if (extendPreviousLine)
                applySceneDrawCurveSegment(resolvedStripIndex, laneIndex, previousClickLineBeat, previousClickLineValue, clickedBeat, normalizedY);
            else
                applySceneDrawPoint(resolvedStripIndex, laneIndex, clickedBeat, normalizedY);

            sceneEditorState.clickLinePending = true;
            sceneEditorState.clickLineStripIndex = resolvedStripIndex;
            sceneEditorState.clickLineLaneIndex = laneIndex;
            sceneEditorState.clickLineBeat = snappedBeat;
            sceneEditorState.clickLineValue = normalizedY;
        };

        if (globalLane >= 0 && globalLayout.laneBounds.contains(e.position))
        {
            placeAutomationClickLine(-1, globalLane, globalLayout.laneBounds);
            return;
        }

        y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }

            if (layout.automationExpanded)
            {
                for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
                {
                    const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                    if (!laneBounds.contains(e.position))
                        continue;

                    placeAutomationClickLine(visibleStrip, lane, laneBounds);
                    return;
                }
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    sceneEditorState.selectedSceneSlot = sceneSlot;
    sceneEditorState.dragActive = false;
    sceneEditorState.dragEventIndex = -1;
    sceneEditorState.dragBaseEvents.clear();
    sceneEditorState.dragTriggerMoveTime = true;
    sceneEditorState.dragTriggerMoveOffset = true;
    sceneEditorState.stepPatternPaintActive = false;
    sceneEditorState.stepPatternPaintStripIndex = -1;
    sceneEditorState.stepPatternPaintLastStep = -1;
    sceneEditorState.stepPatternPaintEnabled = false;
    sceneEditorState.drawActive = false;
    sceneEditorState.drawTriggerLane = false;
    sceneEditorState.drawStripIndex = -1;
    sceneEditorState.drawLaneIndex = -1;
    sceneEditorState.drawLastBeat = 0.0;
    sceneEditorState.drawLastValue = 0.0f;
    sceneEditorState.drawHasLastPoint = false;
    sceneEditorState.eraseActive = false;
    sceneEditorState.eraseTriggerLane = false;
    sceneEditorState.eraseStripIndex = -1;
    sceneEditorState.eraseLaneIndex = -1;
    sceneEditorState.marqueeActive = false;
    sceneEditorState.marqueeRect = {};

    if (hitIndex >= 0 && !processor.isScenePerformanceRecording())
    {
        if (wantsToggleSelection)
        {
            auto indices = sceneEditorState.selectedEventIndices;
            const auto it = std::find(indices.begin(), indices.end(), hitIndex);
            if (it != indices.end())
                indices.erase(it);
            else
                indices.push_back(hitIndex);
            setSceneSelectionIndices(std::move(indices), hitIndex);
        }
        else
        {
            setSceneSelectionIndices({hitIndex}, hitIndex);
            sceneEditorState.dragActive = true;
            sceneEditorState.dragEventIndex = hitIndex;
            sceneEditorState.dragBaseEvents = sceneEditorState.events;
            sceneEditorState.dragTriggerMoveTime = hitTriggerMoveTime;
            sceneEditorState.dragTriggerMoveOffset = hitTriggerMoveOffset;
        }
    }
    else if (!processor.isScenePerformanceRecording() && e.mods.isShiftDown() && !laneActionModifier && !wantsErase)
    {
        sceneEditorState.marqueeActive = true;
        sceneEditorState.marqueeAnchor = e.position;
        sceneEditorState.marqueeRect = juce::Rectangle<float>(e.position.x, e.position.y, 0.0f, 0.0f);
    }
    else if (!wantsToggleSelection)
    {
        clearSceneSelection();
    }

    refreshFromProcessor();
}

void SceneControlPanel::handleSceneTimelineMouseDoubleClick(const juce::MouseEvent& e)
{
    if (processor.isScenePerformanceRecording())
        return;

    grabKeyboardFocus();

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
        0.0f,
        0.0f,
        static_cast<float>(sceneTimelineCanvas.getWidth()),
        sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
        sceneGlobalLaneExpanded);
    const int globalLane = sceneGlobalAutomationLaneIndex();

    if (globalLayout.titleBounds.expanded(10.0f, 4.0f).contains(e.position)
        || globalLayout.laneLabelBounds.expanded(6.0f, 2.0f).contains(e.position))
    {
        sceneGlobalLaneExpanded = !sceneGlobalLaneExpanded;
        updateSceneTimelineContentSize();
        sceneTimelineCanvas.repaint();
        return;
    }

    if (globalLane >= 0)
    {
        for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (!sceneIsGlobalAutomationEvent(event))
                continue;

            const auto marker = sceneControlMarkerBounds(globalLayout.laneBounds, event, lengthBeats);
            if (marker.expanded(3.0f, 3.0f).contains(e.position))
            {
                sceneEditorState.selectedSceneSlot = sceneSlot;
                setSceneSelectionIndices({eventIndex}, eventIndex);
                refreshFromProcessor();
                return;
            }
        }

        if (globalLayout.laneBounds.contains(e.position))
        {
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - globalLayout.laneBounds.getY())
                                                              / juce::jmax(1.0f, globalLayout.laneBounds.getHeight()));
            auto event = makeDefaultSceneControlEventForLane(
                -1,
                globalLane,
                snapSceneBeatToGrid(sceneTimeBeatsForX(globalLayout.laneBounds, e.position.x, lengthBeats), lengthBeats),
                normalizedY,
                sceneDrawModeEnabled);
            auto events = sceneEditorState.events;
            events.push_back(event);
            applyEditedSceneEvents(std::move(events), -1, &event);
            return;
        }
    }

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (cardBounds.expanded(0.0f, 2.0f).contains(e.position))
            sceneEditorState.selectedStripIndex = visibleStrip;

        const bool compactCardHit = layout.compactOverview
            && layout.scenePlaybackAvailable
            && cardBounds.expanded(0.0f, 2.0f).contains(e.position)
            && !layout.stripWriteBounds.contains(e.position)
            && !layout.stripWriteAllBounds.contains(e.position)
            && !layout.stripClearBounds.contains(e.position)
            && !layout.stripDuplicateBounds.contains(e.position)
            && !layout.stripCopyBounds.contains(e.position);

        if (compactCardHit)
        {
            stripHeightExpanded[static_cast<size_t>(visibleStrip)] = true;
            stripAutomationExpanded[static_cast<size_t>(visibleStrip)] = true;
            processor.setSceneEditorStripHeightExpanded(visibleStrip, true);
            processor.setSceneEditorStripAutomationExpanded(visibleStrip, true);
            updateSceneTimelineContentSize();
            sceneTimelineCanvas.repaint();
            return;
        }

        if (layout.titleBounds.expanded(8.0f, 3.0f).contains(e.position))
        {
            stripHeightExpanded[static_cast<size_t>(visibleStrip)]
                = !stripHeightExpanded[static_cast<size_t>(visibleStrip)];
            processor.setSceneEditorStripHeightExpanded(
                visibleStrip,
                stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            updateSceneTimelineContentSize();
            sceneTimelineCanvas.repaint();
            return;
        }

        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }
        if (layout.compactOverview)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }

        for (int eventIndex = static_cast<int>(sceneEditorState.events.size()) - 1; eventIndex >= 0; --eventIndex)
        {
            const auto& event = sceneEditorState.events[static_cast<size_t>(eventIndex)];
            if (event.stripIndex != visibleStrip)
                continue;

            juce::Rectangle<float> marker;
            if (event.type == ScenePerformanceEventType::Trigger)
            {
                marker = sceneTriggerInteractiveBounds(layout, event, lengthBeats);
            }
            else
            {
                if (!layout.automationExpanded)
                    continue;
                marker = sceneControlMarkerBounds(layout, event, lengthBeats);
            }

            if (marker.expanded(3.0f, 3.0f).contains(e.position))
            {
                sceneEditorState.selectedSceneSlot = sceneSlot;
                setSceneSelectionIndices({eventIndex}, eventIndex);
                refreshFromProcessor();
                return;
            }
        }

        const auto triggerInteractionBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
            ? layout.stepLaunchBounds
            : layout.triggerTimelineBounds;

        if (triggerInteractionBounds.contains(e.position))
        {
            ScenePerformanceEvent event;
            event.type = ScenePerformanceEventType::Trigger;
            event.stripIndex = visibleStrip;
            event.timeBeats = snapSceneBeatToGrid(
                sceneTimeBeatsForX(triggerInteractionBounds, e.position.x, lengthBeats),
                lengthBeats);
            event.column = layout.stepTriggerLane
                ? defaultStepTriggerColumnForStrip(visibleStrip)
                : sceneTriggerColumnForY(layout.triggerTimelineBounds, e.position.y);
            event.isNoteOn = true;
            event.sampleSliceId = -1;
            event.sampleStartSample = -1;

            auto events = sceneEditorState.events;
            events.push_back(event);
            applyEditedSceneEvents(std::move(events), -1, &event);
            return;
        }

        if (layout.automationExpanded)
        {
            for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
            {
                const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                if (!laneBounds.contains(e.position))
                    continue;

                const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                              1.0f,
                                                              (e.position.y - laneBounds.getY()) / juce::jmax(1.0f, laneBounds.getHeight()));
                auto event = makeDefaultSceneControlEventForLane(
                    visibleStrip,
                    lane,
                    snapSceneBeatToGrid(sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats), lengthBeats),
                    normalizedY,
                    sceneDrawModeEnabled);
                auto events = sceneEditorState.events;
                events.push_back(event);
                applyEditedSceneEvents(std::move(events), -1, &event);
                return;
            }
        }

        if (sceneDrawModeEnabled
            && e.mods.isAltDown()
            && e.mods.isLeftButtonDown()
            && layout.automationExpanded
            && !processor.isScenePerformanceRecording())
        {
            auto editedEvents = sceneEditorState.events;
            for (int eventIndex = 0; eventIndex < static_cast<int>(editedEvents.size()); ++eventIndex)
            {
                auto& event = editedEvents[static_cast<size_t>(eventIndex)];
                if (event.type != ScenePerformanceEventType::ControlPoint || event.stripIndex != visibleStrip)
                    continue;

                const int laneIndex = sceneAutomationLaneIndex(event);
                if (laneIndex < 0 || laneIndex >= kSceneAutomationLaneCount)
                    continue;

                const auto markerBounds = sceneControlMarkerBounds(layout, event, lengthBeats).expanded(3.0f, 3.0f);
                if (!markerBounds.contains(e.position))
                    continue;

                const float defaultNormalizedValue = sceneAutomationLaneIsBipolar(laneIndex)
                    ? 0.5f
                    : sceneDefaultNormalizedValueForLane(laneIndex);
                event.value = denormalizeSceneAutomationValue(event, defaultNormalizedValue);
                event.column = juce::jlimit(0,
                                            15,
                                            static_cast<int>(std::round(defaultNormalizedValue * 15.0f)));
                const auto preferredEvent = event;
                applyEditedSceneEvents(std::move(editedEvents), eventIndex, &preferredEvent);
                return;
            }
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }
}

void SceneControlPanel::handleSceneTimelineMouseDrag(const juce::MouseEvent& e)
{
    updateSceneHoverState(e.position, true);

    if (sceneEditorState.stepPatternPaintActive)
    {
        const bool focusedSceneIsActive = getFocusedSceneSlot() == processor.getActiveSceneSlot();
        const int paintStripIndex = juce::jlimit(0,
                                                 getVisibleSceneStripCount() - 1,
                                                 sceneEditorState.stepPatternPaintStripIndex);
        float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }
            if (visibleStrip == paintStripIndex && layout.stepPatternBounds.contains(e.position))
            {
                const int absoluteStep = sceneStepAbsoluteStepAtPosition(layout, e.position);
                auto* engine = focusedSceneIsActive ? processor.getAudioEngine() : nullptr;
                auto* strip = engine != nullptr ? engine->getStrip(visibleStrip) : nullptr;
                if (absoluteStep >= 0
                    && absoluteStep != sceneEditorState.stepPatternPaintLastStep
                    && strip != nullptr)
                {
                    strip->setStepEnabledAtIndex(absoluteStep,
                                                 sceneEditorState.stepPatternPaintEnabled,
                                                 true);
                    sceneEditorState.stepPatternPaintLastStep = absoluteStep;
                    refreshFromProcessor();
                    sceneTimelineCanvas.repaint();
                }
                return;
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    if (sceneEditorState.marqueeActive)
    {
        const float x = juce::jmin(sceneEditorState.marqueeAnchor.x, e.position.x);
        const float y = juce::jmin(sceneEditorState.marqueeAnchor.y, e.position.y);
        const float w = std::abs(e.position.x - sceneEditorState.marqueeAnchor.x);
        const float h = std::abs(e.position.y - sceneEditorState.marqueeAnchor.y);
        sceneEditorState.marqueeRect = juce::Rectangle<float>(x, y, w, h);
        auto marqueeSelection = collectSceneEventIndicesInMarquee(sceneEditorState.marqueeRect);
        setSceneSelectionIndices(std::move(marqueeSelection));
        sceneTimelineCanvas.repaint();
        return;
    }

    if (sceneEditorState.eraseActive && !processor.isScenePerformanceRecording())
    {
        const int sceneSlot = getFocusedSceneSlot();
        const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
        const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
            0.0f,
            0.0f,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
            sceneGlobalLaneExpanded);
        const int globalLane = sceneGlobalAutomationLaneIndex();
        if (sceneEditorState.eraseStripIndex < 0
            && globalLane >= 0
            && globalLayout.laneBounds.expanded(0.0f, 3.0f).contains(e.position))
        {
            sceneEditorState.eraseTriggerLane = false;
            sceneEditorState.eraseStripIndex = -1;
            sceneEditorState.eraseLaneIndex = globalLane;
            eraseSceneLaneEventAt(-1,
                                  globalLane,
                                  false,
                                  sceneTimeBeatsForX(globalLayout.laneBounds, e.position.x, lengthBeats));
            return;
        }
        if (sceneEditorState.eraseStripIndex < 0)
            return;

        float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (!layout.scenePlaybackAvailable)
            {
                y += cardBounds.getHeight() + kSceneCardGap;
                continue;
            }
            const auto triggerInteractionBounds = layout.stepTriggerLane && !layout.stepLaunchBounds.isEmpty()
                ? layout.stepLaunchBounds
                : layout.triggerTimelineBounds;
            if (triggerInteractionBounds.expanded(0.0f, 3.0f).contains(e.position))
            {
                sceneEditorState.eraseTriggerLane = true;
                sceneEditorState.eraseStripIndex = visibleStrip;
                sceneEditorState.eraseLaneIndex = -1;
                eraseSceneLaneEventAt(visibleStrip,
                                      -1,
                                      true,
                                      sceneTimeBeatsForX(triggerInteractionBounds, e.position.x, lengthBeats));
                return;
            }

            if (layout.automationExpanded)
            {
                for (int lane = 0; lane < kSceneAutomationLaneCount; ++lane)
                {
                    const auto laneBounds = layout.automationTimelineBounds[static_cast<size_t>(lane)];
                    if (!laneBounds.expanded(0.0f, 3.0f).contains(e.position))
                        continue;

                    sceneEditorState.eraseTriggerLane = false;
                    sceneEditorState.eraseStripIndex = visibleStrip;
                    sceneEditorState.eraseLaneIndex = lane;
                    eraseSceneLaneEventAt(visibleStrip,
                                          lane,
                                          false,
                                          sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats));
                    return;
                }
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }
    }

    if (sceneEditorState.drawActive && sceneDrawModeEnabled && !processor.isScenePerformanceRecording())
    {
        const int sceneSlot = getFocusedSceneSlot();
        const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
        const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
            0.0f,
            0.0f,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
            sceneGlobalLaneExpanded);
        const int globalLane = sceneGlobalAutomationLaneIndex();
        if (sceneEditorState.drawStripIndex < 0
            && globalLane >= 0
            && sceneEditorState.drawLaneIndex == globalLane)
        {
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - globalLayout.laneBounds.getY())
                                                              / juce::jmax(1.0f, globalLayout.laneBounds.getHeight()));
            const double currentBeat = sceneTimeBeatsForX(globalLayout.laneBounds, e.position.x, lengthBeats);
            const double startBeat = sceneEditorState.drawHasLastPoint ? sceneEditorState.drawLastBeat : currentBeat;
            const float startValue = sceneEditorState.drawHasLastPoint ? sceneEditorState.drawLastValue : normalizedY;
            applySceneDrawCurveSegment(-1,
                                       globalLane,
                                       startBeat,
                                       startValue,
                                       currentBeat,
                                       normalizedY);
            sceneEditorState.drawLastBeat = currentBeat;
            sceneEditorState.drawLastValue = normalizedY;
            sceneEditorState.drawHasLastPoint = true;
            return;
        }
        if (sceneEditorState.drawStripIndex < 0)
            return;

        const int lockedStripIndex = juce::jlimit(0,
                                                  getVisibleSceneStripCount() - 1,
                                                  sceneEditorState.drawStripIndex);
        float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
        SceneStripCardLayout lockedLayout;
        bool foundLockedLayout = false;
        for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
        {
            const auto cardBounds = juce::Rectangle<float>(
                0.0f,
                y,
                static_cast<float>(sceneTimelineCanvas.getWidth()),
                sceneStripCardHeight(processor,
                                     visibleStrip,
                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
            const auto layout = makeSceneStripCardLayout(processor,
                                                         visibleStrip,
                                                         cardBounds,
                                                         stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                         stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
            if (visibleStrip == lockedStripIndex)
            {
                lockedLayout = layout;
                foundLockedLayout = true;
                break;
            }

            y += cardBounds.getHeight() + kSceneCardGap;
        }

        if (!foundLockedLayout || !lockedLayout.scenePlaybackAvailable)
            return;

        if (sceneEditorState.drawTriggerLane)
        {
            const auto triggerInteractionBounds = lockedLayout.stepTriggerLane && !lockedLayout.stepLaunchBounds.isEmpty()
                ? lockedLayout.stepLaunchBounds
                : lockedLayout.triggerTimelineBounds;
            sceneEditorState.drawHasLastPoint = false;
            applySceneDrawTrigger(lockedStripIndex,
                                  sceneTimeBeatsForX(triggerInteractionBounds, e.position.x, lengthBeats),
                                  lockedLayout.stepTriggerLane
                                      ? defaultStepTriggerColumnForStrip(lockedStripIndex)
                                      : sceneTriggerColumnForY(lockedLayout.triggerTimelineBounds, e.position.y));
            return;
        }

        const int lockedLaneIndex = sceneEditorState.drawLaneIndex;
        if (!juce::isPositiveAndBelow(lockedLaneIndex, kSceneAutomationLaneCount)
            || !lockedLayout.automationExpanded)
        {
            return;
        }

        const auto laneBounds = lockedLayout.automationTimelineBounds[static_cast<size_t>(lockedLaneIndex)];
        const float normalizedY = (e.mods.isAltDown() && sceneAutomationLaneIsBipolar(lockedLaneIndex))
            ? 0.5f
            : (1.0f - juce::jlimit(0.0f,
                                   1.0f,
                                   (e.position.y - laneBounds.getY()) / juce::jmax(1.0f, laneBounds.getHeight())));
        const double currentBeat = sceneTimeBeatsForX(laneBounds, e.position.x, lengthBeats);
        const double startBeat = sceneEditorState.drawHasLastPoint ? sceneEditorState.drawLastBeat : currentBeat;
        const float startValue = sceneEditorState.drawHasLastPoint ? sceneEditorState.drawLastValue : normalizedY;
        applySceneDrawCurveSegment(lockedStripIndex,
                                   lockedLaneIndex,
                                   startBeat,
                                   startValue,
                                   currentBeat,
                                   normalizedY);
        sceneEditorState.drawLastBeat = currentBeat;
        sceneEditorState.drawLastValue = normalizedY;
        sceneEditorState.drawHasLastPoint = true;
        return;
    }

    if (!sceneEditorState.dragActive
        || processor.isScenePerformanceRecording()
        || sceneEditorState.dragEventIndex < 0
        || sceneEditorState.dragEventIndex >= static_cast<int>(sceneEditorState.dragBaseEvents.size()))
    {
        return;
    }

    const int sceneSlot = getFocusedSceneSlot();
    const double lengthBeats = getSceneTimelineLengthBeats(sceneSlot);
    auto editedEvents = sceneEditorState.dragBaseEvents;
    auto editedEvent = editedEvents[static_cast<size_t>(sceneEditorState.dragEventIndex)];
    const auto globalLayout = makeSceneGlobalLaneLayout(juce::Rectangle<float>(
        0.0f,
        0.0f,
        static_cast<float>(sceneTimelineCanvas.getWidth()),
        sceneGlobalLaneCardHeight(sceneGlobalLaneExpanded)),
        sceneGlobalLaneExpanded);
    const bool draggingGlobalLane = sceneIsGlobalAutomationEvent(editedEvent);

    float y = sceneGlobalLaneSectionHeight(sceneGlobalLaneExpanded);
    const int originalStrip = draggingGlobalLane
        ? -1
        : juce::jlimit(0, getVisibleSceneStripCount() - 1, editedEvent.stripIndex);
    int hoveredStrip = originalStrip;
    SceneStripCardLayout hoveredLayout;
    bool foundHoveredLayout = draggingGlobalLane;
    for (int visibleStrip = 0; visibleStrip < getVisibleSceneStripCount(); ++visibleStrip)
    {
        const auto cardBounds = juce::Rectangle<float>(
            0.0f,
            y,
            static_cast<float>(sceneTimelineCanvas.getWidth()),
            sceneStripCardHeight(processor,
                                 visibleStrip,
                                 stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                 stripHeightExpanded[static_cast<size_t>(visibleStrip)]));
        const auto layout = makeSceneStripCardLayout(processor,
                                                     visibleStrip,
                                                     cardBounds,
                                                     stripAutomationExpanded[static_cast<size_t>(visibleStrip)],
                                                     stripHeightExpanded[static_cast<size_t>(visibleStrip)]);
        if (!layout.scenePlaybackAvailable)
        {
            y += cardBounds.getHeight() + kSceneCardGap;
            continue;
        }
        if (visibleStrip == originalStrip)
            hoveredLayout = layout;

        if (cardBounds.expanded(0.0f, 4.0f).contains(e.position))
        {
            if (layout.automationExpanded)
            {
                hoveredStrip = visibleStrip;
                hoveredLayout = layout;
                foundHoveredLayout = true;
            }
            break;
        }

        y += cardBounds.getHeight() + kSceneCardGap;
    }

    if (editedEvent.type == ScenePerformanceEventType::Trigger)
    {
        bool moveTriggerTime = sceneEditorState.dragTriggerMoveTime;
        bool moveTriggerOffset = sceneEditorState.dragTriggerMoveOffset;
        if (e.mods.isShiftDown() && !e.mods.isAltDown())
            moveTriggerOffset = false;
        else if (e.mods.isAltDown() && !e.mods.isShiftDown())
            moveTriggerTime = false;
        if (!moveTriggerTime && !moveTriggerOffset)
        {
            moveTriggerTime = sceneEditorState.dragTriggerMoveTime;
            moveTriggerOffset = sceneEditorState.dragTriggerMoveOffset;
        }
        if (!foundHoveredLayout || !hoveredLayout.automationExpanded)
            return;
        editedEvent.stripIndex = hoveredStrip;
        const auto timelineBounds = hoveredLayout.stepTriggerLane && !hoveredLayout.stepLaunchBounds.isEmpty()
            ? hoveredLayout.stepLaunchBounds
            : hoveredLayout.triggerTimelineBounds;
        if (moveTriggerTime)
        {
            const float normalizedX = juce::jlimit(0.0f,
                                                   1.0f,
                                                   (e.position.x - timelineBounds.getX()) / juce::jmax(1.0f, timelineBounds.getWidth()));
            editedEvent.timeBeats = snapSceneBeatToGrid(static_cast<double>(normalizedX) * lengthBeats, lengthBeats);
        }
        if (moveTriggerOffset)
        {
            const int updatedColumn = sceneTriggerColumnForY(timelineBounds, e.position.y);
            if (updatedColumn != editedEvent.column)
            {
                editedEvent.column = updatedColumn;
                editedEvent.sampleSliceId = -1;
                editedEvent.sampleStartSample = -1;
            }
        }
    }
    else
    {
        const int laneIndex = sceneAutomationLaneIndex(editedEvent);
        if (laneIndex < 0 || laneIndex >= kSceneAutomationLaneCount)
            return;
        if (sceneAutomationLaneUsesGlobalStrip(laneIndex))
        {
            editedEvent.stripIndex = -1;
            const auto laneBounds = globalLayout.laneBounds;
            const float normalizedX = juce::jlimit(0.0f,
                                                   1.0f,
                                                   (e.position.x - laneBounds.getX()) / juce::jmax(1.0f, laneBounds.getWidth()));
            editedEvent.timeBeats = snapSceneBeatToGrid(static_cast<double>(normalizedX) * lengthBeats, lengthBeats);
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - laneBounds.getY()) / juce::jmax(1.0f, laneBounds.getHeight()));
            editedEvent.value = denormalizeSceneAutomationValue(editedEvent, normalizedY);
            editedEvent.column = juce::jlimit(0, 15, static_cast<int>(std::round(normalizedY * 15.0f)));
        }
        else
        {
            if (!hoveredLayout.automationExpanded && !foundHoveredLayout)
                return;
            if (!hoveredLayout.automationExpanded)
                return;

            editedEvent.stripIndex = hoveredStrip;
            const auto laneBounds = hoveredLayout.automationTimelineBounds[static_cast<size_t>(laneIndex)];
            const float normalizedX = juce::jlimit(0.0f,
                                                   1.0f,
                                                   (e.position.x - laneBounds.getX()) / juce::jmax(1.0f, laneBounds.getWidth()));
            editedEvent.timeBeats = snapSceneBeatToGrid(static_cast<double>(normalizedX) * lengthBeats, lengthBeats);
            const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                          1.0f,
                                                          (e.position.y - laneBounds.getY()) / juce::jmax(1.0f, laneBounds.getHeight()));
            editedEvent.value = denormalizeSceneAutomationValue(editedEvent, normalizedY);
            editedEvent.column = juce::jlimit(0, 15, static_cast<int>(std::round(normalizedY * 15.0f)));
        }
    }

    editedEvents[static_cast<size_t>(sceneEditorState.dragEventIndex)] = editedEvent;
    applyEditedSceneEvents(std::move(editedEvents), -1, &editedEvent);
}

void SceneControlPanel::handleSceneTimelineMouseMove(const juce::MouseEvent& e)
{
    updateSceneHoverState(e.position, true);
}

void SceneControlPanel::handleSceneTimelineMouseExit(const juce::MouseEvent&)
{
    clearSceneHoverState();
}

void SceneControlPanel::handleSceneTimelineMouseUp(const juce::MouseEvent&)
{
    const bool shouldRepaint = sceneEditorState.marqueeActive || !sceneEditorState.marqueeRect.isEmpty();
    sceneEditorState.dragActive = false;
    sceneEditorState.dragEventIndex = -1;
    sceneEditorState.dragBaseEvents.clear();
    sceneEditorState.dragTriggerMoveTime = true;
    sceneEditorState.dragTriggerMoveOffset = true;
    sceneEditorState.stepPatternPaintActive = false;
    sceneEditorState.stepPatternPaintStripIndex = -1;
    sceneEditorState.stepPatternPaintLastStep = -1;
    sceneEditorState.stepPatternPaintEnabled = false;
    sceneEditorState.drawActive = false;
    sceneEditorState.drawTriggerLane = false;
    sceneEditorState.drawStripIndex = -1;
    sceneEditorState.drawLaneIndex = -1;
    sceneEditorState.drawLastBeat = 0.0;
    sceneEditorState.drawLastValue = 0.0f;
    sceneEditorState.drawHasLastPoint = false;
    sceneEditorState.eraseActive = false;
    sceneEditorState.eraseTriggerLane = false;
    sceneEditorState.eraseStripIndex = -1;
    sceneEditorState.eraseLaneIndex = -1;
    sceneEditorState.marqueeActive = false;
    sceneEditorState.marqueeRect = {};
    if (shouldRepaint)
        sceneTimelineCanvas.repaint();
}

void SceneControlPanel::handleSceneTimelineMouseWheel(const juce::MouseEvent& e,
                                                      const juce::MouseWheelDetails& wheel)
{
    if (sceneEditorState.dragActive
        || sceneEditorState.drawActive
        || sceneEditorState.eraseActive
        || sceneEditorState.marqueeActive
        || sceneEditorState.stepPatternPaintActive)
    {
        return;
    }

    const bool horizontalGesture = std::abs(wheel.deltaX) > std::abs(wheel.deltaY)
        && std::abs(wheel.deltaX) > 0.001f;
    if (horizontalGesture)
    {
        const int deltaX = static_cast<int>(std::round(-wheel.deltaX * 96.0f));
        if (deltaX != 0)
        {
            const int maxX = juce::jmax(0, sceneTimelineCanvas.getWidth() - sceneViewport.getWidth());
            sceneViewport.setViewPosition(juce::jlimit(0,
                                                       maxX,
                                                       sceneViewport.getViewPositionX() + deltaX),
                                          sceneViewport.getViewPositionY());
        }
        return;
    }

    if (std::abs(wheel.deltaY) < 0.001f)
        return;

    if (!e.mods.isShiftDown())
    {
        const int deltaY = static_cast<int>(std::round(-wheel.deltaY * 96.0f));
        if (deltaY != 0)
        {
            const int maxY = juce::jmax(0, sceneTimelineCanvas.getHeight() - sceneViewport.getHeight());
            sceneViewport.setViewPosition(sceneViewport.getViewPositionX(),
                                          juce::jlimit(0,
                                                       maxY,
                                                       sceneViewport.getViewPositionY() + deltaY));
        }
        return;
    }

    static constexpr std::array<int, 7> kZoomChoices{ 1, 2, 3, 4, 5, 6, 8 };
    int zoomIndex = 0;
    for (int i = 0; i < static_cast<int>(kZoomChoices.size()); ++i)
    {
        if (kZoomChoices[static_cast<size_t>(i)] == sceneZoomFactor)
        {
            zoomIndex = i;
            break;
        }
    }

    const int nextZoomIndex = juce::jlimit(0,
                                           static_cast<int>(kZoomChoices.size()) - 1,
                                           zoomIndex + (wheel.deltaY > 0.0f ? 1 : -1));
    const int nextZoomFactor = kZoomChoices[static_cast<size_t>(nextZoomIndex)];
    if (nextZoomFactor == sceneZoomFactor)
        return;

    const int previousWidth = juce::jmax(1, sceneTimelineCanvas.getWidth());
    const int previousViewY = sceneViewport.getViewPositionY();
    const float viewportAnchorX = e.getEventRelativeTo(&sceneViewport).position.x;
    const float anchorNorm = juce::jlimit(0.0f,
                                          1.0f,
                                          e.position.x / static_cast<float>(previousWidth));

    sceneZoomFactor = nextZoomFactor;
    processor.setSceneEditorZoomFactor(sceneZoomFactor);
    sceneZoomBox.setSelectedId(sceneZoomFactor, juce::dontSendNotification);
    updateSceneTimelineContentSize();

    const int newWidth = juce::jmax(1, sceneTimelineCanvas.getWidth());
    const int anchorContentX = static_cast<int>(std::round(anchorNorm * static_cast<float>(newWidth)));
    const int maxX = juce::jmax(0, newWidth - sceneViewport.getWidth());
    const int targetX = juce::jlimit(0,
                                     maxX,
                                     anchorContentX - static_cast<int>(std::round(viewportAnchorX)));
    sceneViewport.setViewPosition(targetX, previousViewY);
    sceneTimelineCanvas.repaint();
}

void SceneControlPanel::mouseDown(const juce::MouseEvent& e)
{
    auto sceneSlotForComponent = [this](const juce::Component* component) -> int
    {
        for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
        {
            if (component == &sceneSelectorButtons[static_cast<size_t>(sceneSlot)])
                return sceneSlot;
        }
        return -1;
    };

    const int sceneSlot = sceneSlotForComponent(e.eventComponent);
    if (sceneSlot < 0 || !e.mods.isLeftButtonDown())
    {
        sceneSlotDragSource = -1;
        sceneSlotDragTarget = -1;
        sceneChainExternalDropStep = -1;
        sceneSlotDragMoved = false;
        sceneSlotDragSuppressClick = false;
        return;
    }

    sceneSlotDragSource = sceneSlot;
    sceneSlotDragTarget = e.mods.isAltDown() ? sceneSlot : -1;
    sceneChainExternalDropStep = -1;
    sceneSlotDragMoved = false;
    sceneSlotDragSuppressClick = false;
}

void SceneControlPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (sceneSlotDragSource < 0)
        return;

    if (e.getDistanceFromDragStart() > 3)
        sceneSlotDragMoved = true;

    int externalDropStep = -1;
    const int chainLength = processor.getSceneChainLength();
    if (sceneSlotDragMoved && chainLength < MlrVSTAudioProcessor::MaxSceneChainSteps)
    {
        const auto layout = makeSceneChainLayout(sceneChainCanvas.getLocalBounds().toFloat());
        const auto chainLocalPosition = e.getEventRelativeTo(&sceneChainCanvas).position;
        externalDropStep = sceneChainInsertStepAtPosition(layout, chainLocalPosition, chainLength);
    }

    int hoveredSlot = -1;
    if (sceneSlotDragMoved && externalDropStep < 0 && e.mods.isAltDown())
    {
        const auto screenPosition = e.getScreenPosition();
        for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
        {
            if (sceneSelectorButtons[static_cast<size_t>(sceneSlot)].getScreenBounds().contains(screenPosition))
            {
                hoveredSlot = sceneSlot;
                break;
            }
        }
    }

    sceneSlotDragTarget = hoveredSlot;
    if (externalDropStep != sceneChainExternalDropStep)
    {
        sceneChainExternalDropStep = externalDropStep;
        sceneChainCanvas.repaint();
    }
}

void SceneControlPanel::mouseUp(const juce::MouseEvent& e)
{
    const int sourceSceneSlot = sceneSlotDragSource;
    const int duplicateTarget = sceneSlotDragTarget;
    const int externalDropStep = sceneChainExternalDropStep;
    const bool dragMoved = sceneSlotDragMoved;
    bool stateChanged = false;

    if (sourceSceneSlot >= 0 && dragMoved)
    {
        if (externalDropStep >= 0)
        {
            auto chainSteps = snapshotSceneChainSteps(processor);
            if (static_cast<int>(chainSteps.size()) < MlrVSTAudioProcessor::MaxSceneChainSteps)
            {
                MlrVSTAudioProcessor::SceneChainStep newStep;
                newStep.sceneSlot = sourceSceneSlot;
                newStep.repeats = 1;
                const int insertIndex = juce::jlimit(0, static_cast<int>(chainSteps.size()), externalDropStep);
                chainSteps.insert(chainSteps.begin() + insertIndex, newStep);
                const bool loopEnabled = processor.isSceneChainLoopEnabled();
                const int loopStart = processor.getSceneChainLoopStartStep();
                const int loopEnd = processor.getSceneChainLoopEndStep();
                applySceneChainSteps(processor, chainSteps, loopEnabled, loopStart, loopEnd);
                selectedSceneActionSlot = sourceSceneSlot;
                processor.focusSceneSlot(sourceSceneSlot);
                stateChanged = true;
            }
        }
        else if (e.mods.isAltDown() && duplicateTarget >= 0 && sourceSceneSlot != duplicateTarget)
        {
            processor.duplicateScenePerformanceClip(sourceSceneSlot, duplicateTarget);
            stateChanged = true;
        }
    }

    sceneSlotDragSuppressClick = (sourceSceneSlot >= 0 && dragMoved);
    sceneSlotDragSource = -1;
    sceneSlotDragTarget = -1;
    sceneChainExternalDropStep = -1;
    sceneSlotDragMoved = false;
    sceneChainCanvas.repaint();

    if (stateChanged)
        refreshFromProcessor();
}

//==============================================================================
// PresetControlPanel Implementation
//==============================================================================

PresetControlPanel::PresetControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    instructionsLabel.setText("Click=load default/preset  Shift+Click=save  Right-click=delete  Init=resets runtime", juce::dontSendNotification);
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

    initButton.setButtonText("Init");
    initButton.onClick = [this]()
    {
        processor.initRuntimeStateToDefaults();
        presetNameDraft = processor.presetExists(selectedPresetIndex)
            ? processor.getPresetName(selectedPresetIndex)
            : juce::String();
        presetNameEditor.setText(presetNameDraft, juce::dontSendNotification);
        updatePresetButtons();
    };
    initButton.setTooltip("Reset the current runtime state to defaults and clear all loaded sample buffers.");
    addAndMakeVisible(initButton);
    styleUiButton(initButton);

    saveButton.setButtonText("Save");
    saveButton.onClick = [this]()
    {
        auto safePanel = juce::Component::SafePointer<PresetControlPanel>(this);
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
            {
                loadPresetClicked(i);
            }
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
    const int initButtonW = 52;
    const int exportButtonW = 78;
    deleteButton.setBounds(editorArea.removeFromRight(saveDeleteButtonW));
    editorArea.removeFromRight(4);
    exportWavButton.setBounds(editorArea.removeFromRight(exportButtonW));
    editorArea.removeFromRight(4);
    saveButton.setBounds(editorArea.removeFromRight(saveDeleteButtonW));
    editorArea.removeFromRight(6);
    initButton.setBounds(editorArea.removeFromRight(initButtonW));
    editorArea.removeFromRight(6);

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
    const auto trimmed = (typedName.isNotEmpty() ? typedName : presetNameEditor.getText()).trim();
    if (trimmed.isNotEmpty())
    {
        processor.setPresetName(index, trimmed);
        presetNameDraft = trimmed;
        presetNameEditor.setText(trimmed, juce::dontSendNotification);
    }

    processor.savePreset(index);
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

    for (int i = 0; i < MlrVSTAudioProcessor::MaxPresetSlots; ++i)
    {
        bool exists = processor.presetExists(i);
        auto& button = presetButtons[static_cast<size_t>(i)];
        const juce::String presetName = exists ? processor.getPresetName(i) : juce::String();
        button.setButtonText(makePresetBubbleLabel(presetName, i));
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
// MacroControlPanel Implementation
//==============================================================================

MacroControlPanel::MacroControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    knobLookAndFeel.setKnobColor(kAccent);

    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
    {
        auto& macro = macros[static_cast<size_t>(i)];
        macro.label.setText("M" + juce::String(i + 1), juce::dontSendNotification);
        macro.label.setJustificationType(juce::Justification::centred);
        macro.label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        macro.label.setColour(juce::Label::textColourId, kTextPrimary);
        addAndMakeVisible(macro.label);

        macro.ccButton.setButtonText(macroCcLabelText(processor.getMacroMidiCc(i)));
        macro.ccButton.setTooltip("Click to arm MIDI learn for this macro. Shift-click to reset to the default MPK mini CC.");
        macro.ccButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        macro.ccButton.setTriggeredOnMouseDown(true);
        styleUiButton(macro.ccButton);
        macro.ccButton.onClick = [this, i]()
        {
            if (juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown())
            {
                processor.resetMacroMidiCcToDefault(i);
            }
            else if (processor.getMacroMidiLearnIndex() == i)
            {
                processor.cancelMacroMidiLearn();
            }
            else
            {
                processor.beginMacroMidiLearn(i);
            }

            refreshFromProcessor();
            repaint();
        };
        addAndMakeVisible(macro.ccButton);

        populateMacroTargetBox(macro.targetBox);
        styleUiCombo(macro.targetBox);
        macro.targetBox.setTooltip("Select which strip parameter this macro knob controls.");
        macro.targetBox.onChange = [this, i]()
        {
            processor.setMacroTarget(i, comboIdToMacroTargetSelection(macros[static_cast<size_t>(i)].targetBox.getSelectedId()));
            refreshFromProcessor();
            repaint();
        };
        addAndMakeVisible(macro.targetBox);

        macro.slider.setLookAndFeel(&knobLookAndFeel);
        macro.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        macro.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        macro.slider.setRange(0.0, 1.0, 0.001);
        macro.slider.setValue(MlrVSTAudioProcessor::getDefaultMacroNormalizedValue(processor.getMacroTarget(i)), juce::dontSendNotification);
        enableAltClickReset(macro.slider, MlrVSTAudioProcessor::getDefaultMacroNormalizedValue(processor.getMacroTarget(i)));
        macro.slider.setPopupDisplayEnabled(true, false, this);
        macro.slider.textFromValueFunction = [this, i](double value)
        {
            return macroValueText(processor.getMacroTarget(i), static_cast<float>(value));
        };
        macro.slider.onValueChange = [this, i]()
        {
            if (isRefreshing)
                return;
            processor.setSelectedStripMacroValue(i, static_cast<float>(macros[static_cast<size_t>(i)].slider.getValue()));
        };
        macro.slider.interceptMouseDown = [this, i](const juce::MouseEvent& e)
        {
            if (!e.mods.isPopupMenu())
                return false;

            juce::PopupMenu menu;
            const bool isLearning = (processor.getMacroMidiLearnIndex() == i);
            menu.addItem(1, isLearning ? "Cancel MIDI Learn" : "Learn MIDI CC");
            menu.addItem(2, "Reset to Default CC");

            const int result = menu.showAt(&macros[static_cast<size_t>(i)].slider);
            if (result == 1)
            {
                if (isLearning)
                    processor.cancelMacroMidiLearn();
                else
                    processor.beginMacroMidiLearn(i);
            }
            else if (result == 2)
            {
                processor.resetMacroMidiCcToDefault(i);
            }

            if (result != 0)
            {
                refreshFromProcessor();
                repaint();
            }

            return true;
        };
        macro.slider.setTooltip("Drag to control the selected strip. Right-click for MIDI learn options.");
        addAndMakeVisible(macro.slider);
    }

    refreshFromProcessor();
}

void MacroControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void MacroControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(6, 0);
    bounds.removeFromBottom(1);
    const int columns = 4;
    const int rows = 2;
    const int gapX = 6;
    const int gapY = 1;
    const int cellWidth = (bounds.getWidth() - (gapX * (columns - 1))) / columns;
    const int cellHeight = (bounds.getHeight() - (gapY * (rows - 1))) / rows;

    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
    {
        const int row = i / columns;
        const int columnIndex = i % columns;
        auto column = juce::Rectangle<int>(
            bounds.getX() + columnIndex * (cellWidth + gapX),
            bounds.getY() + row * (cellHeight + gapY),
            cellWidth,
            cellHeight);
        auto& macro = macros[static_cast<size_t>(i)];
        macro.label.setBounds(column.removeFromTop(11));
        auto assignmentRow = column.removeFromTop(15);
        macro.ccButton.setBounds(assignmentRow.removeFromLeft(40));
        assignmentRow.removeFromLeft(3);
        macro.targetBox.setBounds(assignmentRow);
        column.removeFromTop(1);
        macro.slider.setBounds(column.reduced(1, 0));
    }
}

void MacroControlPanel::refreshFromProcessor()
{
    const auto state = processor.getMacroState();
    const auto targetColour = state.hasTargetStrip ? getStripColor(state.stripIndex) : kTextMuted;
    knobLookAndFeel.setKnobColor(targetColour);

    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
    {
        auto& macro = macros[static_cast<size_t>(i)];
        const bool isLearning = (processor.getMacroMidiLearnIndex() == i);
        const auto target = processor.getMacroTarget(i);
        const bool hasTarget = target != MlrVSTAudioProcessor::MacroTarget::None;
        const auto knobColour = targetColour.withMultipliedSaturation(1.0f);
        macro.label.setColour(juce::Label::textColourId, hasTarget ? targetColour : kTextMuted);
        macro.targetBox.setSelectedId(macroTargetToComboId(target), juce::dontSendNotification);
        macro.slider.setEnabled(state.hasTargetStrip && hasTarget);
        macro.slider.setColour(juce::Slider::rotarySliderFillColourId, knobColour.withAlpha(0.88f));
        macro.slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff232323));
        macro.slider.setColour(juce::Slider::thumbColourId, knobColour.brighter(0.18f));
        macro.slider.setColour(juce::Slider::textBoxTextColourId, kTextPrimary);
        macro.slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff4a4d50));
        macro.slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff2c3034));
        macro.slider.setDoubleClickReturnValue(true, MlrVSTAudioProcessor::getDefaultMacroNormalizedValue(target));
        macro.slider.setTooltip(!hasTarget
                                    ? "Assign a target, then drag to control the selected strip. Right-click for MIDI learn options."
                                    : (MlrVSTAudioProcessor::macroTargetWritesToSceneLane(target)
                                           ? "Drag to control the selected strip. In Scene recording, this target writes into a scene lane. Right-click for MIDI learn options."
                                           : "Drag to control the selected strip. In Scene recording, this target stays live-only and is not written into a scene lane. Right-click for MIDI learn options."));
        macro.ccButton.setButtonText(isLearning
                                         ? "LEARN..."
                                         : macroCcLabelText(processor.getMacroMidiCc(i)));
        macro.ccButton.setColour(juce::TextButton::buttonColourId,
                                 isLearning ? targetColour.withAlpha(0.92f) : juce::Colour(0xff2d3135));
        macro.ccButton.setColour(juce::TextButton::buttonOnColourId,
                                 isLearning ? targetColour.withAlpha(0.98f) : juce::Colour(0xff3b4148));
        macro.ccButton.setColour(juce::TextButton::textColourOffId,
                                 isLearning ? juce::Colour(0xff101214) : kTextMuted);
        macro.ccButton.setColour(juce::TextButton::textColourOnId,
                                 isLearning ? juce::Colour(0xff101214) : kTextPrimary);
    }

    isRefreshing = true;
    for (int i = 0; i < MlrVSTAudioProcessor::MacroCount; ++i)
        macros[static_cast<size_t>(i)].slider.setValue(state.values[static_cast<size_t>(i)], juce::dontSendNotification);
    isRefreshing = false;
}

//==============================================================================
// GlobalControlPanel Implementation
//==============================================================================

GlobalControlPanel::GlobalControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    versionLabel.setText("v" + juce::String(MLRVST_BUILD_VERSION), juce::dontSendNotification);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    versionLabel.setColour(juce::Label::textColourId, kTextMuted);
    versionLabel.setTooltip("Plugin version. Build " + juce::String(MLRVST_BUILD_STAMP) + ".");
    addChildComponent(versionLabel);

    masterVolumeLabel.setText("Master", juce::dontSendNotification);
    masterVolumeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(masterVolumeLabel);

    masterVolumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    masterVolumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    masterVolumeSlider.setRange(0.0, 1.0, 0.01);
    masterVolumeSlider.setValue(1.0);
    enableAltClickReset(masterVolumeSlider, 1.0);
    masterVolumeSlider.setPopupDisplayEnabled(true, false, this);
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
    quantizeSelector.setSelectedId(6);
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
        const int selectedIndex = juce::jmax(0, innerLoopLengthBox.getSelectedId() - 1);
        processor.setInnerLoopLengthSelection(selectedIndex);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    swingDivisionLabel.setText("Swing Grid", juce::dontSendNotification);
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

    pitchControlModeLabel.setText("Pitch", juce::dontSendNotification);
    pitchControlModeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchControlModeLabel);

    pitchControlModeBox.addItem("Pitch Shift", 1);
    pitchControlModeBox.addItem("SoundTouch", 2);
    pitchControlModeBox.addItem("Resample", 3);
    pitchControlModeBox.addItem("Signalsmith", 4);
    pitchControlModeBox.addItem("Bungee", 5);
    pitchControlModeBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(pitchControlModeBox);
    styleUiCombo(pitchControlModeBox);
    pitchControlModeBox.setTooltip(
        "Global strip pitch behavior: Pitch Shift, SoundTouch, Signalsmith, Bungee, or playback-rate Resample. "
        "SoundTouch, Signalsmith, and Bungee use cached/hybrid pitch paths. Signalsmith is not currently available in Sample mode, "
        "so Sample strips use the local Pitch Shift path instead.");
    pitchControlModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "pitchControlMode", pitchControlModeBox);
    pitchControlModeBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    flipTempoMatchModeLabel.setText("Tempo Match", juce::dontSendNotification);
    flipTempoMatchModeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(flipTempoMatchModeLabel);

    flipTempoMatchModeBox.addItem("Repitch", 1);
    flipTempoMatchModeBox.addItem("Stretch", 2);
    flipTempoMatchModeBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(flipTempoMatchModeBox);
    styleUiCombo(flipTempoMatchModeBox);
    flipTempoMatchModeBox.setTooltip("Loop/Gate/One-shot/Grain host-tempo matching. Repitch keeps the current PPQ-safe loop path. Stretch follows the current Stretch selection.");
    flipTempoMatchModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "flipTempoMatchMode", flipTempoMatchModeBox);
    flipTempoMatchModeBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    rootNoteLabel.setText("Root Note", juce::dontSendNotification);
    rootNoteLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(rootNoteLabel);

    for (int i = 0; i < 12; ++i)
        rootNoteBox.addItem(getPitchClassName(i), i + 1);
    rootNoteBox.setSelectedId(processor.getGlobalRootNotePitchClass() + 1, juce::dontSendNotification);
    addAndMakeVisible(rootNoteBox);
    styleUiCombo(rootNoteBox);
    rootNoteBox.setTooltip("Global target root note. When a PM strip is active, PM owns this value and PS strips follow it.");
    rootNoteBox.onChange = [this]()
    {
        processor.setGlobalRootNotePitchClass(rootNoteBox.getSelectedId() - 1);
    };

    globalScaleLabel.setText("Scale", juce::dontSendNotification);
    globalScaleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(globalScaleLabel);

    globalScaleBox.addItem("None", 1);
    globalScaleBox.addItem("Major", 2);
    globalScaleBox.addItem("Minor", 3);
    globalScaleBox.addItem("Dorian", 4);
    globalScaleBox.addItem("Pentatonic", 5);
    globalScaleBox.setSelectedId(pitchScaleToComboId(processor.getGlobalPitchScale()), juce::dontSendNotification);
    addAndMakeVisible(globalScaleBox);
    styleUiCombo(globalScaleBox);
    globalScaleBox.setTooltip("Global target scale. None leaves pitch sliders free; the musical scales quantize pitch controls against this tonal world.");
    globalScaleBox.onChange = [this]()
    {
        processor.setGlobalPitchScale(comboIdToPitchScale(globalScaleBox.getSelectedId()));
    };

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

    inputMonitorLabel.setText("Input", juce::dontSendNotification);
    inputMonitorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(inputMonitorLabel);

    inputMonitorSlider.setSliderStyle(juce::Slider::LinearVertical);
    inputMonitorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    inputMonitorSlider.setRange(0.0, 1.0, 0.01);
    inputMonitorSlider.setValue(0.0);
    enableAltClickReset(inputMonitorSlider, 1.0);
    inputMonitorSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(inputMonitorSlider);
    inputMonitorSlider.setTooltip("Monitor live input signal level.");

    inputMonitorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "inputMonitor", inputMonitorSlider);
    inputMonitorSlider.onDragEnd = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    inputMeterLabel.setText("L   R", juce::dontSendNotification);
    inputMeterLabel.setJustificationType(juce::Justification::centred);
    inputMeterLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    addAndMakeVisible(inputMeterLabel);

    addAndMakeVisible(inputMeterL);
    addAndMakeVisible(inputMeterR);

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
    triggerFadeInSlider.setRange(0.01, 120.0, 0.01);
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

    stretchBackendLabel.setText("Stretch", juce::dontSendNotification);
    stretchBackendLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(stretchBackendLabel);

    stretchBackendBox.addItem("Resample", 1);
    stretchBackendBox.addItem("SoundTouch", 2);
    stretchBackendBox.addItem("Bungee", 3);
    stretchBackendBox.setTooltip("Tempo/pitch backend for Loop/Gate swing, non-Flip strip timestretch work, and Tempo Match when set to Stretch. In loop mode, Bungee is still the dedicated pitch-preserving tempo-match path.");
    styleUiCombo(stretchBackendBox);
    addAndMakeVisible(stretchBackendBox);
    stretchBackendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "stretchBackend", stretchBackendBox);
    stretchBackendBox.onChange = [this]()
    {
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    transientMethodLabel.setText("Transients", juce::dontSendNotification);
    transientMethodLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(transientMethodLabel);

    transientMethodBox.addItem("Hybrid", 1);
    transientMethodBox.addItem("HFC", 2);
    transientMethodBox.addItem("Flux", 3);
    transientMethodBox.setTooltip("Transient onset detector family. Hybrid blends HFC and spectral flux, HFC favors drums, Flux favors broader attacks.");
    styleUiCombo(transientMethodBox);
    addAndMakeVisible(transientMethodBox);
    transientMethodAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "transientOnsetMethod", transientMethodBox);
    transientMethodBox.onChange = [this]()
    {
        processor.syncTransientDetectionSettingsFromParameters(true);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    transientSensitivityLabel.setText("Sens", juce::dontSendNotification);
    transientSensitivityLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(transientSensitivityLabel);

    transientSensitivityBox.addItem("VLow", 1);
    transientSensitivityBox.addItem("Low", 2);
    transientSensitivityBox.addItem("Norm", 3);
    transientSensitivityBox.addItem("High", 4);
    transientSensitivityBox.addItem("VHigh", 5);
    transientSensitivityBox.setTooltip("How easily attacks are detected. Higher values find more transients and quieter hits.");
    styleUiCombo(transientSensitivityBox);
    addAndMakeVisible(transientSensitivityBox);
    transientSensitivityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "transientSensitivity", transientSensitivityBox);
    transientSensitivityBox.onChange = [this]()
    {
        processor.syncTransientDetectionSettingsFromParameters(true);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    transientSnapLabel.setText("Snap", juce::dontSendNotification);
    transientSnapLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(transientSnapLabel);

    transientSnapBox.addItem("Soft", 1);
    transientSnapBox.addItem("Loose", 2);
    transientSnapBox.addItem("Norm", 3);
    transientSnapBox.addItem("Tight", 4);
    transientSnapBox.addItem("Exact", 5);
    transientSnapBox.setTooltip("How aggressively slice markers pull onto the attack start once an onset is found.");
    styleUiCombo(transientSnapBox);
    addAndMakeVisible(transientSnapBox);
    transientSnapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "transientSnap", transientSnapBox);
    transientSnapBox.onChange = [this]()
    {
        processor.syncTransientDetectionSettingsFromParameters(true);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    transientSpacingLabel.setText("Space", juce::dontSendNotification);
    transientSpacingLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(transientSpacingLabel);

    transientSpacingBox.addItem("Tight", 1);
    transientSpacingBox.addItem("Close", 2);
    transientSpacingBox.addItem("Norm", 3);
    transientSpacingBox.addItem("Wide", 4);
    transientSpacingBox.addItem("Wider", 5);
    transientSpacingBox.setTooltip("Minimum spacing between transient slices. Wider avoids clustering, tighter allows closer markers.");
    styleUiCombo(transientSpacingBox);
    addAndMakeVisible(transientSpacingBox);
    transientSpacingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "transientSpacing", transientSpacingBox);
    transientSpacingBox.onChange = [this]()
    {
        processor.syncTransientDetectionSettingsFromParameters(true);
        if (globalUiReady)
            processor.markPersistentGlobalUserChange();
    };

    continuousTraversalToggle.setButtonText("Full Slices");
    continuousTraversalToggle.setClickingTogglesState(true);
    continuousTraversalToggle.setTooltip("On keeps high speeds moving through every slice. Off restores the older skip-style traversal.");
    addAndMakeVisible(continuousTraversalToggle);
    styleUiButton(continuousTraversalToggle);
    continuousTraversalAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "continuousTraversal", continuousTraversalToggle);
    continuousTraversalToggle.onClick = [this]()
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

void GlobalControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void GlobalControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(6, 5);

    auto titleRow = bounds.removeFromTop(16);
    tooltipsToggle.setBounds(titleRow.removeFromRight(86));
    titleRow.removeFromRight(6);
    momentaryToggle.setBounds(titleRow.removeFromRight(92));
    versionLabel.setBounds({});
    titleLabel.setBounds({});

    bounds.removeFromTop(1);
    auto controlsArea = bounds;

    const int labelHeight = 14;
    const int controlGap = 6;
    const int sectionGap = 8;
    const int sliderWidth = 42;
    const int utilityColumnWidth = 118;
    const int meterWidth = 24;
    const int knobWidth = 80;
    const int meterVerticalInset = 6;

    auto layoutTallControl = [](juce::Rectangle<int> area, juce::Label& label, juce::Component& control)
    {
        label.setBounds(area.removeFromTop(labelHeight));
        area.removeFromTop(2);
        control.setBounds(area);
    };

    auto layoutAlignedComboCell = [](juce::Rectangle<int> area,
                                     juce::Label& label,
                                     juce::ComboBox& box,
                                     int labelWidth,
                                     int preferredBoxWidth)
    {
        label.setJustificationType(juce::Justification::centredLeft);
        const int gap = 6;
        const int minBoxWidth = 54;
        const int clampedLabelWidth = juce::jlimit(32, juce::jmax(32, area.getWidth() - minBoxWidth - gap), labelWidth);
        label.setBounds(area.removeFromLeft(clampedLabelWidth));
        area.removeFromLeft(gap);
        const int boxWidth = juce::jlimit(minBoxWidth, area.getWidth(), preferredBoxWidth);
        box.setBounds(area.removeFromLeft(boxWidth).withTrimmedTop(1).withTrimmedBottom(1));
    };

    auto gainArea = controlsArea.removeFromLeft((sliderWidth * 2) + utilityColumnWidth + meterWidth + (controlGap * 3));
    controlsArea.removeFromLeft(sectionGap);
    auto knobArea = controlsArea.removeFromLeft((knobWidth * 2) + controlGap);
    controlsArea.removeFromLeft(sectionGap);
    auto comboArea = controlsArea;

    auto masterArea = gainArea.removeFromLeft(sliderWidth);
    gainArea.removeFromLeft(controlGap);
    auto utilityArea = gainArea.removeFromLeft(utilityColumnWidth);
    gainArea.removeFromLeft(controlGap);
    auto inputArea = gainArea.removeFromLeft(sliderWidth);
    gainArea.removeFromLeft(controlGap);
    auto meterArea = gainArea;

    layoutTallControl(masterArea, masterVolumeLabel, masterVolumeSlider);
    layoutTallControl(inputArea, inputMonitorLabel, inputMonitorSlider);

    utilityArea.removeFromTop(labelHeight + 2);
    limiterToggle.setBounds(utilityArea.removeFromTop(24));
    utilityArea.removeFromTop(6);
    outputRoutingLabel.setBounds(utilityArea.removeFromTop(labelHeight));
    utilityArea.removeFromTop(2);
    outputRoutingBox.setBounds(utilityArea.removeFromTop(22).withTrimmedTop(1).withTrimmedBottom(1));
    utilityArea.removeFromTop(6);
    continuousTraversalToggle.setBounds(utilityArea.removeFromTop(24));

    inputMeterLabel.setBounds(meterArea.removeFromTop(labelHeight));
    meterArea.removeFromTop(2);
    auto leftMeter = meterArea.removeFromLeft(meterArea.getWidth() / 2);
    inputMeterL.setBounds(leftMeter.reduced(1, meterVerticalInset));
    inputMeterR.setBounds(meterArea.reduced(1, meterVerticalInset));

    auto crossfadeArea = knobArea.removeFromLeft(knobWidth);
    knobArea.removeFromLeft(controlGap);
    auto triggerFadeArea = knobArea;
    layoutTallControl(crossfadeArea, crossfadeLengthLabel, crossfadeLengthSlider);
    layoutTallControl(triggerFadeArea, triggerFadeInLabel, triggerFadeInSlider);

    const int rowGap = 4;
    const int columnGap = 8;
    const int comboRowHeight = 26;
    auto timingRow = comboArea.removeFromTop(comboRowHeight);
    comboArea.removeFromTop(rowGap);
    auto policyRow = comboArea.removeFromTop(comboRowHeight);
    comboArea.removeFromTop(rowGap);
    auto musicalRow = comboArea.removeFromTop(comboRowHeight);
    comboArea.removeFromTop(rowGap);
    auto transientRow = comboArea.removeFromTop(comboRowHeight);

    auto measureLabelWidth = [](juce::Label& label)
    {
        return juce::GlyphArrangement::getStringWidthInt(label.getFont(), label.getText()) + 12;
    };

    auto sharedColumnsArea = comboArea;
    const int sharedColumnWidth = juce::jmax(86, (sharedColumnsArea.getWidth() - (columnGap * 2)) / 3);
    auto column1 = sharedColumnsArea.removeFromLeft(sharedColumnWidth);
    sharedColumnsArea.removeFromLeft(columnGap);
    auto column2 = sharedColumnsArea.removeFromLeft(sharedColumnWidth);
    sharedColumnsArea.removeFromLeft(columnGap);
    auto column3 = sharedColumnsArea;

    const int column1LabelWidth = juce::jmax(measureLabelWidth(quantizeLabel),
                                             measureLabelWidth(flipTempoMatchModeLabel),
                                             measureLabelWidth(rootNoteLabel));
    const int column2LabelWidth = juce::jmax(measureLabelWidth(innerLoopLengthLabel),
                                             measureLabelWidth(pitchControlModeLabel),
                                             measureLabelWidth(globalScaleLabel));
    const int column3LabelWidth = juce::jmax(measureLabelWidth(swingDivisionLabel),
                                             measureLabelWidth(stretchBackendLabel),
                                             measureLabelWidth(qualityLabel));

    const int column1BoxWidth = 98;
    const int column2BoxWidth = 102;
    const int column3BoxWidth = 112;

    layoutAlignedComboCell({ column1.getX(), timingRow.getY(), column1.getWidth(), timingRow.getHeight() },
                           quantizeLabel, quantizeSelector, column1LabelWidth, column1BoxWidth);
    layoutAlignedComboCell({ column2.getX(), timingRow.getY(), column2.getWidth(), timingRow.getHeight() },
                           innerLoopLengthLabel, innerLoopLengthBox, column2LabelWidth, column2BoxWidth);
    layoutAlignedComboCell({ column3.getX(), timingRow.getY(), column3.getWidth(), timingRow.getHeight() },
                           swingDivisionLabel, swingDivisionBox, column3LabelWidth, column3BoxWidth);

    layoutAlignedComboCell({ column1.getX(), policyRow.getY(), column1.getWidth(), policyRow.getHeight() },
                           flipTempoMatchModeLabel, flipTempoMatchModeBox, column1LabelWidth, column1BoxWidth);
    layoutAlignedComboCell({ column2.getX(), policyRow.getY(), column2.getWidth(), policyRow.getHeight() },
                           pitchControlModeLabel, pitchControlModeBox, column2LabelWidth, column2BoxWidth);
    layoutAlignedComboCell({ column3.getX(), policyRow.getY(), column3.getWidth(), policyRow.getHeight() },
                           stretchBackendLabel, stretchBackendBox, column3LabelWidth, column3BoxWidth);

    layoutAlignedComboCell({ column1.getX(), musicalRow.getY(), column1.getWidth(), musicalRow.getHeight() },
                           rootNoteLabel, rootNoteBox, column1LabelWidth, column1BoxWidth);
    layoutAlignedComboCell({ column2.getX(), musicalRow.getY(), column2.getWidth(), musicalRow.getHeight() },
                           globalScaleLabel, globalScaleBox, column2LabelWidth, column2BoxWidth);
    layoutAlignedComboCell({ column3.getX(), musicalRow.getY(), column3.getWidth(), musicalRow.getHeight() },
                           qualityLabel, resamplingQualityBox, column3LabelWidth, column3BoxWidth);

    auto transientCells = transientRow;
    const int transientGap = 6;
    const int transientCellWidth = juce::jmax(76, (transientCells.getWidth() - (transientGap * 3)) / 4);
    auto transientCell1 = transientCells.removeFromLeft(transientCellWidth);
    transientCells.removeFromLeft(transientGap);
    auto transientCell2 = transientCells.removeFromLeft(transientCellWidth);
    transientCells.removeFromLeft(transientGap);
    auto transientCell3 = transientCells.removeFromLeft(transientCellWidth);
    transientCells.removeFromLeft(transientGap);
    auto transientCell4 = transientCells;

    const int transientMethodLabelWidth = measureLabelWidth(transientMethodLabel);
    const int transientSensitivityLabelWidth = measureLabelWidth(transientSensitivityLabel);
    const int transientSnapLabelWidth = measureLabelWidth(transientSnapLabel);
    const int transientSpacingLabelWidth = measureLabelWidth(transientSpacingLabel);

    layoutAlignedComboCell(transientCell1, transientMethodLabel, transientMethodBox, transientMethodLabelWidth, 74);
    layoutAlignedComboCell(transientCell2, transientSensitivityLabel, transientSensitivityBox, transientSensitivityLabelWidth, 72);
    layoutAlignedComboCell(transientCell3, transientSnapLabel, transientSnapBox, transientSnapLabelWidth, 72);
    layoutAlignedComboCell(transientCell4, transientSpacingLabel, transientSpacingBox, transientSpacingLabelWidth, 72);
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
    continuousTraversalToggle.setToggleState(processor.usesContinuousTraversal(), juce::dontSendNotification);
    rootNoteBox.setSelectedId(processor.getGlobalRootNotePitchClass() + 1, juce::dontSendNotification);
    const bool pitchMasterActive = processor.isLoopPitchMasterActive();
    rootNoteBox.setEnabled(!pitchMasterActive);
    rootNoteBox.setTooltip(pitchMasterActive
                               ? "PM is active, so the root note is currently driven by the Pitch Master strip."
                               : "Global target root note. PS strips follow this target tonal center.");
    globalScaleBox.setSelectedId(pitchScaleToComboId(processor.getGlobalPitchScale()), juce::dontSendNotification);
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
                return;
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
        const bool hideSceneOwnedPage = processor.isSceneModeEnabled()
            && modeAtButton == MlrVSTAudioProcessor::ControlMode::GroupAssign;
        const bool isActive = (activeMode == modeAtButton) && (activeMode != MlrVSTAudioProcessor::ControlMode::Normal);

        row.positionLabel.setVisible(!hideSceneOwnedPage);
        row.modeButton.setVisible(!hideSceneOwnedPage);
        row.upButton.setVisible(!hideSceneOwnedPage);
        row.downButton.setVisible(!hideSceneOwnedPage);
        if (hideSceneOwnedPage)
            continue;
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
        const juce::String presetName = exists ? processor.getPresetName(i) : juce::String();
        button.setButtonText(makePresetBubbleLabel(presetName, i));
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
