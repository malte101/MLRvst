#include "SampleMode.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>

#include <juce_dsp/juce_dsp.h>

#if MLRVST_ENABLE_ESSENTIA_NATIVE
 #include <essentia/algorithmfactory.h>
 #include <essentia/essentia.h>
 #include <essentia/essentiamath.h>
#endif

namespace
{
constexpr double kInitialSliceTargetSeconds = 1.5;
constexpr int kMaxInitialSliceCount = 256;
constexpr int kDefaultSliceFadeSamples = 96;
constexpr int kMaxStoredTransientCount = 512;
constexpr float kMinViewZoom = 1.0f;
constexpr float kMaxViewZoom = 32.0f;
constexpr float kCueHitRadiusPixels = 8.0f;
constexpr float kVisibleSliceMarkerHitRadiusPixels = 16.0f;
constexpr float kDetailedWaveformZoomThreshold = 3.0f;
constexpr int kDetailedWaveformMaxVisibleSamples = 240000;
#if !MLRVST_ENABLE_ESSENTIA_NATIVE
constexpr const char* kEssentiaScriptName = "mlrvst_flip_essentia_analysis.py";
constexpr const char* kAnalysisPythonPackageName = "essentia";
#endif
constexpr float kKeyLockCacheRateTolerance = 0.0005f;
constexpr float kKeyLockCachePitchTolerance = 0.01f;

#if !MLRVST_ENABLE_ESSENTIA_NATIVE
struct EssentiaPythonConfig
{
    bool valid = false;
    juce::String executable;
    juce::String pythonPath;
    juce::String pathEnv;
    juce::String failureReason;
};

int compareVersionTag(const juce::String& lhs, const juce::String& rhs)
{
    const auto leftParts = juce::StringArray::fromTokens(lhs, ".", "");
    const auto rightParts = juce::StringArray::fromTokens(rhs, ".", "");
    const int maxParts = juce::jmax(leftParts.size(), rightParts.size());
    for (int i = 0; i < maxParts; ++i)
    {
        const int left = i < leftParts.size() ? leftParts[i].getIntValue() : 0;
        const int right = i < rightParts.size() ? rightParts[i].getIntValue() : 0;
        if (left != right)
            return left < right ? -1 : 1;
    }
    return 0;
}

juce::String sanitizeEssentiaFailureReason(juce::String reason)
{
    reason = reason.trim();
    if (reason.isEmpty())
        return "ES unavailable";
    reason = reason.replaceCharacters("\r\n\t", "   ");
    while (reason.contains("  "))
        reason = reason.replace("  ", " ");
    if (reason.containsIgnoreCase("No module named 'essentia'")
        || reason.containsIgnoreCase("No module named \"essentia\""))
        return "ES import failed";
    if (reason.containsIgnoreCase("timed out"))
        return "ES timeout";
    if (reason.containsIgnoreCase("can't open file"))
        return "ES script launch failed";
    return "ES " + reason.upToFirstOccurrenceOf("Traceback", false, false).trim().substring(0, 64);
}

bool pythonSiteContainsPackage(const juce::File& sitePackagesDir)
{
    return sitePackagesDir.getChildFile(kAnalysisPythonPackageName).isDirectory()
        || sitePackagesDir.getChildFile(kAnalysisPythonPackageName).getChildFile("__init__.py").existsAsFile();
}

juce::String findNewestEssentiaSitePackages()
{
    juce::String bestVersion;
    juce::String bestSitePackages;

    auto considerSiteDir = [&](const juce::File& sitePackagesDir, const juce::String& version)
    {
        if (!sitePackagesDir.isDirectory() || !pythonSiteContainsPackage(sitePackagesDir))
            return;

        if (bestVersion.isEmpty() || compareVersionTag(version, bestVersion) > 0)
        {
            bestVersion = version;
            bestSitePackages = sitePackagesDir.getFullPathName();
        }
    };

    const auto userPythonRoot = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Python");
    if (userPythonRoot.isDirectory())
    {
        const auto children = userPythonRoot.findChildFiles(juce::File::findDirectories, false);
        for (const auto& child : children)
        {
            const auto version = child.getFileName();
            considerSiteDir(child.getChildFile("lib").getChildFile("python").getChildFile("site-packages"), version);
        }
    }

    for (const auto& rootPath : { juce::String("/opt/homebrew/lib"), juce::String("/usr/local/lib") })
    {
        const juce::File root(rootPath);
        if (!root.isDirectory())
            continue;

        const auto children = root.findChildFiles(juce::File::findDirectories, false, "python*");
        for (const auto& child : children)
        {
            const auto version = child.getFileName().fromFirstOccurrenceOf("python", false, false);
            considerSiteDir(child.getChildFile("site-packages"), version);
        }
    }

    return bestSitePackages;
}

juce::String extractPythonVersionFromUserSite(const juce::String& userSite)
{
    const int libIndex = userSite.indexOfIgnoreCase("/lib/python");
    if (libIndex < 0)
        return {};

    const int versionStart = libIndex + juce::String("/lib/python").length();
    const int versionEnd = userSite.indexOfIgnoreCase(versionStart, "/");
    if (versionEnd <= versionStart)
        return {};

    return userSite.substring(versionStart, versionEnd);
}

bool probeEssentiaPythonExecutable(const juce::String& executable,
                                   const juce::String& pythonPath,
                                   const juce::String& pathEnv,
                                   juce::String& failureReason)
{
    juce::ChildProcess probe;
    juce::StringArray args;
    args.add("/usr/bin/env");
    args.add("HOME=" + juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName());
    args.add("PATH=" + pathEnv);
    if (pythonPath.isNotEmpty())
        args.add("PYTHONPATH=" + pythonPath);
    args.add("MPLCONFIGDIR=" + juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("mlrvst_essentia_mpl")
                                   .getFullPathName());
    args.add("XDG_CACHE_HOME=" + juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName());
    args.add(executable);
    args.add("-c");
    args.add("import essentia, essentia.standard, sys; print(sys.executable)");

    if (!probe.start(args))
    {
        failureReason = "could not start " + executable;
        return false;
    }

    if (!probe.waitForProcessToFinish(20000))
    {
        probe.kill();
        failureReason = "timed out probing " + executable;
        return false;
    }

    const auto output = probe.readAllProcessOutput().trim();
    const auto lines = juce::StringArray::fromLines(output);
    const auto lastLine = lines.isEmpty() ? output : lines[lines.size() - 1].trim();
    if (lastLine == executable || lastLine.endsWithIgnoreCase(juce::File(executable).getFileName()))
        return true;

    failureReason = output.isNotEmpty() ? output : ("probe failed for " + executable);
    return false;
}

EssentiaPythonConfig resolveEssentiaPythonConfig()
{
    EssentiaPythonConfig config;
    const auto userSite = findNewestEssentiaSitePackages();

    juce::StringArray candidates;
    const auto envOverride = juce::SystemStats::getEnvironmentVariable("MLRVST_ESSENTIA_PYTHON", {});
    if (envOverride.isNotEmpty())
        candidates.add(envOverride);

    const auto version = extractPythonVersionFromUserSite(userSite);
    if (version.isNotEmpty())
    {
        candidates.add("/opt/homebrew/opt/python@" + version + "/bin/python" + version);
        candidates.add("/opt/homebrew/bin/python" + version);
        candidates.add("/usr/local/bin/python" + version);
        candidates.add("python" + version);
    }

    candidates.add("/opt/homebrew/bin/python3");
    candidates.add("/usr/local/bin/python3");
    candidates.add("python3");
    candidates.removeDuplicates(false);

    juce::String pathEnv;
    pathEnv << "/opt/homebrew/opt/python@" << (version.isNotEmpty() ? version : "3.12") << "/bin"
            << ":/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    juce::String lastFailure = userSite.isEmpty()
        ? juce::String("ES package not found in Python site-packages")
        : juce::String();
    for (const auto& candidate : candidates)
    {
        juce::String failure;
        if (probeEssentiaPythonExecutable(candidate, userSite, pathEnv, failure))
        {
            config.valid = true;
            config.executable = candidate;
            config.pythonPath = userSite;
            config.pathEnv = pathEnv;
            return config;
        }
        if (failure.isNotEmpty())
            lastFailure = failure;
    }

    config.failureReason = sanitizeEssentiaFailureReason(lastFailure);
    return config;
}

const EssentiaPythonConfig& getEssentiaPythonConfig()
{
    static const EssentiaPythonConfig config = resolveEssentiaPythonConfig();
    return config;
}
#endif

juce::String buildSliceLabel(int index);
float safeNormalizedPosition(int64_t samplePosition, int64_t totalSamples);
void fillSliceMetadata(SampleSlice& slice, int sliceIndex, int64_t totalSamples);

std::array<SampleSlice, SliceModel::VisibleSliceCount> buildDistributedRandomSliceSelection(
    const std::vector<SampleSlice>& allSlices,
    int64_t totalSamples,
    std::mt19937& rng)
{
    std::array<SampleSlice, SliceModel::VisibleSliceCount> selection {};
    if (allSlices.empty())
        return selection;

    const int selectionCount = juce::jmin(SliceModel::VisibleSliceCount, static_cast<int>(allSlices.size()));
    if (selectionCount <= 0)
        return selection;

    const int64_t safeTotalSamples = juce::jmax<int64_t>(
        1,
        totalSamples > 0 ? totalSamples : juce::jmax<int64_t>(1, allSlices.back().endSample));
    const auto sliceCount = static_cast<int>(allSlices.size());
    const int minIndexGap = juce::jmax(1, sliceCount / juce::jmax(1, selectionCount * 3));
    std::vector<int> chosenIndices;
    chosenIndices.reserve(static_cast<size_t>(selectionCount));
    std::vector<bool> used(allSlices.size(), false);

    auto respectsGap = [&](int candidateIndex) -> bool
    {
        for (const int chosenIndex : chosenIndices)
        {
            if (std::abs(candidateIndex - chosenIndex) < minIndexGap)
                return false;
        }
        return true;
    };

    std::uniform_real_distribution<double> bucketJitter(0.15, 0.85);

    for (int slot = 0; slot < selectionCount; ++slot)
    {
        const double bucketStartNorm = static_cast<double>(slot) / static_cast<double>(selectionCount);
        const double bucketEndNorm = static_cast<double>(slot + 1) / static_cast<double>(selectionCount);
        const int64_t bucketStartSample = static_cast<int64_t>(std::floor(bucketStartNorm * safeTotalSamples));
        const int64_t bucketEndSample = static_cast<int64_t>(std::ceil(bucketEndNorm * safeTotalSamples));
        const int64_t targetSample = static_cast<int64_t>(
            std::floor(((static_cast<double>(slot) + bucketJitter(rng))
                         / static_cast<double>(selectionCount))
                        * safeTotalSamples));

        std::vector<int> bucketCandidates;
        bucketCandidates.reserve(allSlices.size());
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            if (used[static_cast<size_t>(i)])
                continue;

            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            if (slice.startSample >= bucketStartSample
                && slice.startSample < bucketEndSample
                && respectsGap(i))
            {
                bucketCandidates.push_back(i);
            }
        }

        int chosenIndex = -1;
        if (!bucketCandidates.empty())
        {
            std::uniform_int_distribution<int> pick(0, static_cast<int>(bucketCandidates.size()) - 1);
            chosenIndex = bucketCandidates[static_cast<size_t>(pick(rng))];
        }
        else
        {
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int pass = 0; pass < 2 && chosenIndex < 0; ++pass)
            {
                const bool enforceGap = (pass == 0);
                for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
                {
                    if (used[static_cast<size_t>(i)])
                        continue;
                    if (enforceGap && !respectsGap(i))
                        continue;

                    const auto& slice = allSlices[static_cast<size_t>(i)];
                    if (slice.id < 0 || slice.endSample <= slice.startSample)
                        continue;

                    const int64_t distance = std::abs(slice.startSample - targetSample);
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        chosenIndex = i;
                    }
                }
            }
        }

        if (chosenIndex < 0 || chosenIndex >= static_cast<int>(allSlices.size()))
            continue;

        used[static_cast<size_t>(chosenIndex)] = true;
        chosenIndices.push_back(chosenIndex);
        selection[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(chosenIndex)];
    }

    return selection;
}

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

juce::String analysisBackendDisplayText(bool essentiaUsed, const juce::String& analysisSource)
{
    const auto source = analysisSource.trim();

    if (essentiaUsed)
    {
        if (source.containsIgnoreCase("pitch unresolved"))
            return "Essentia partial";
        return "Essentia OK";
    }

    if (source.startsWithIgnoreCase("manual tempo"))
        return "Manual tempo";

    if (source.containsIgnoreCase("import failed"))
        return "Internal fallback";
    if (source.containsIgnoreCase("timeout"))
        return "Internal fallback";
    if (source.containsIgnoreCase("script launch failed"))
        return "Internal fallback";
    if (source.containsIgnoreCase("invalid json")
        || source.containsIgnoreCase("invalid result")
        || source.containsIgnoreCase("empty output"))
        return "Internal fallback";
    if (source.startsWithIgnoreCase("ES "))
        return "Internal fallback";
    if (source.containsIgnoreCase("essentia"))
        return "Internal fallback";
    if (source.containsIgnoreCase("internal"))
        return "Internal fallback";

    return "Analysis fallback";
}

juce::String compactLoadStatusText(const juce::String& statusText)
{
    const auto source = statusText.trim();
    if (source.isEmpty())
        return {};

    if (source.startsWithIgnoreCase("Loading "))
        return "Loading...";
    if (source.containsIgnoreCase("Decoding"))
        return "Decoding...";
    if (source.containsIgnoreCase("Building waveform"))
        return "Waveform...";
    if (source.containsIgnoreCase("Analyzing transients"))
        return "Transients...";
    if (source.containsIgnoreCase("Essentia tempo / pitch"))
        return "Essentia...";
    if (source.containsIgnoreCase("Snapping loop-style transients"))
        return "Snapping...";
    if (source.containsIgnoreCase("Internal analysis fallback"))
        return "Internal fallback...";
    if (source.containsIgnoreCase("Finalizing analysis"))
        return "Finalizing...";

    if (source.length() > 28)
        return source.substring(0, 28).trimEnd() + "...";

    return source;
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

double meanAbsoluteLevelInRange(const std::vector<double>& absolutePrefixSum, int startSample, int endSample)
{
    const int clampedStart = juce::jmax(0, startSample);
    const int clampedEnd = juce::jmax(clampedStart, juce::jmin(static_cast<int>(absolutePrefixSum.size()) - 1, endSample));
    const int sampleCount = juce::jmax(1, clampedEnd - clampedStart);
    return (absolutePrefixSum[static_cast<size_t>(clampedEnd)] - absolutePrefixSum[static_cast<size_t>(clampedStart)])
        / static_cast<double>(sampleCount);
}

std::vector<int64_t> refineOnsetSamplesToLeadingEdges(const juce::AudioBuffer<float>& monoBuffer,
                                                      const std::vector<int64_t>& onsetSamples,
                                                      int frameSize,
                                                      int hopSize)
{
    std::vector<int64_t> refined;
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 2 || onsetSamples.empty())
        return refined;

    const float* mono = monoBuffer.getReadPointer(0);
    std::vector<double> absolutePrefixSum(static_cast<size_t>(totalSamples) + 1u, 0.0);
    for (int i = 0; i < totalSamples; ++i)
        absolutePrefixSum[static_cast<size_t>(i) + 1u] = absolutePrefixSum[static_cast<size_t>(i)] + std::abs(mono[i]);

    const int searchBehind = juce::jlimit(24, juce::jmax(24, totalSamples / 256), frameSize / 2);
    const int searchAhead = juce::jlimit(8, juce::jmax(8, totalSamples / 1024), juce::jmax(8, hopSize / 3));
    const int lookBehind = juce::jlimit(3, juce::jmax(3, totalSamples / 4096), juce::jmax(4, hopSize / 8));
    const int lookAhead = juce::jlimit(6, juce::jmax(6, totalSamples / 2048), juce::jmax(6, hopSize / 5));

    refined.reserve(onsetSamples.size());
    for (const auto onsetSample : onsetSamples)
    {
        const int center = juce::jlimit(1, totalSamples - 2, static_cast<int>(onsetSample));
        const int searchStart = juce::jmax(1, center - searchBehind);
        const int searchEnd = juce::jmin(totalSamples - 2, center + searchAhead);
        if (searchEnd <= searchStart)
        {
            refined.push_back(center);
            continue;
        }

        std::vector<double> scores(static_cast<size_t>(searchEnd - searchStart + 1), 0.0);
        int bestIndex = center;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (int sampleIndex = searchStart; sampleIndex <= searchEnd; ++sampleIndex)
        {
            const double levelBefore = meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex - lookBehind, sampleIndex);
            const double levelAfter = meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex, sampleIndex + lookAhead);
            const double levelRise = juce::jmax(0.0, levelAfter - levelBefore);
            const double attackStep = std::abs(mono[sampleIndex] - mono[sampleIndex - 1])
                + (0.5 * std::abs(mono[sampleIndex + 1] - mono[sampleIndex]));
            const int delta = sampleIndex - center;
            const double proximityPenalty = delta > 0
                ? 0.0048 * static_cast<double>(delta)
                : 0.00045 * static_cast<double>(-delta);
            const double score = (levelRise * 8.0) + (attackStep * 2.0) - proximityPenalty;
            scores[static_cast<size_t>(sampleIndex - searchStart)] = score;

            if (score > bestScore)
            {
                bestScore = score;
                bestIndex = sampleIndex;
            }
        }

        int refinedIndex = bestIndex;
        if (bestScore > 0.0)
        {
            const double threshold = bestScore * 0.46;
            for (int sampleIndex = searchStart; sampleIndex <= bestIndex; ++sampleIndex)
            {
                const double score = scores[static_cast<size_t>(sampleIndex - searchStart)];
                if (score >= threshold)
                {
                    refinedIndex = sampleIndex;
                    break;
                }
            }
        }

        refined.push_back(static_cast<int64_t>(refinedIndex));
    }

    std::sort(refined.begin(), refined.end());
    refined.erase(std::unique(refined.begin(), refined.end()), refined.end());
    return refined;
}

std::array<int64_t, SliceModel::VisibleSliceCount> buildLoopStyleAnchorStarts(int64_t totalSamples,
                                                                               const std::vector<int64_t>& onsetSamples)
{
    std::array<int64_t, SliceModel::VisibleSliceCount> starts {};
    const int64_t safeTotalSamples = juce::jmax<int64_t>(1, totalSamples);
    const int64_t lastSample = safeTotalSamples - 1;

    if (onsetSamples.empty())
    {
        for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
            starts[static_cast<size_t>(i)] = juce::jlimit<int64_t>(0,
                                                                   lastSample,
                                                                   (static_cast<int64_t>(i) * safeTotalSamples)
                                                                       / SliceModel::VisibleSliceCount);
        return starts;
    }

    const int lastIndex = SliceModel::VisibleSliceCount - 1;
    const int64_t nominalStep = juce::jmax<int64_t>(1,
        static_cast<int64_t>(std::llround(static_cast<double>(lastSample)
                                          / static_cast<double>(juce::jmax(1, lastIndex)))));
    const int64_t maxSnapDistance = juce::jmax<int64_t>(1, (nominalStep * 45) / 100);
    for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
    {
        if (i == 0)
        {
            starts[static_cast<size_t>(i)] = 0;
            continue;
        }

        const int64_t target = juce::jlimit<int64_t>(0,
                                                     lastSample,
                                                     static_cast<int64_t>((static_cast<double>(i)
                                                         / static_cast<double>(lastIndex))
                                                         * static_cast<double>(lastSample)));

        int64_t chosen = target;
        auto it = std::lower_bound(onsetSamples.begin(), onsetSamples.end(), target);

        int64_t bestCandidate = chosen;
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        if (it != onsetSamples.end())
        {
            const int64_t dist = std::abs(*it - target);
            if (dist < bestDistance)
            {
                bestDistance = dist;
                bestCandidate = *it;
            }
        }
        if (it != onsetSamples.begin())
        {
            const int64_t prev = *std::prev(it);
            const int64_t dist = std::abs(prev - target);
            if (dist < bestDistance)
            {
                bestDistance = dist;
                bestCandidate = prev;
            }
        }

        if (bestDistance < std::numeric_limits<int64_t>::max() && bestDistance <= maxSnapDistance)
            chosen = bestCandidate;

        chosen = juce::jmax(starts[static_cast<size_t>(i - 1)] + 1, chosen);
        starts[static_cast<size_t>(i)] = juce::jlimit<int64_t>(0, lastSample, chosen);
    }

    return starts;
}

std::vector<int64_t> detectLoopStyleTransientSamples(const juce::AudioBuffer<float>& monoBuffer,
                                                     double sampleRate)
{
    std::vector<int64_t> transientSamples;
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 0 || sampleRate <= 0.0)
        return transientSamples;

    int fftOrder = 8;
    while ((1 << fftOrder) < juce::jmin(2048, totalSamples) && fftOrder < 12)
        ++fftOrder;
    const int frameSize = 1 << fftOrder;
    const int hopSize = juce::jmax(32, frameSize / 8);
    const int frames = juce::jmax(1, 1 + ((totalSamples - frameSize) / hopSize));
    if (frames < 4)
        return transientSamples;

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(static_cast<size_t>(frameSize),
                                               juce::dsp::WindowingFunction<float>::hann,
                                               true);

    const float* mono = monoBuffer.getReadPointer(0);
    const int halfBins = frameSize / 2;
    std::vector<float> fftData(static_cast<size_t>(2 * frameSize), 0.0f);
    std::vector<float> prevMag(static_cast<size_t>(halfBins), 0.0f);
    std::vector<float> spectralFlux(static_cast<size_t>(frames), 0.0f);
    std::vector<float> frameEnergy(static_cast<size_t>(frames), 0.0f);

    for (int frame = 0; frame < frames; ++frame)
    {
        const int start = frame * hopSize;
        double energy = 0.0;

        for (int n = 0; n < frameSize; ++n)
        {
            const int sampleIndex = juce::jlimit(0, totalSamples - 1, start + n);
            const float sample = mono[sampleIndex];
            fftData[static_cast<size_t>(n)] = sample;
            energy += static_cast<double>(sample * sample);
        }

        for (int n = frameSize; n < 2 * frameSize; ++n)
            fftData[static_cast<size_t>(n)] = 0.0f;

        window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(frameSize));
        fft.performFrequencyOnlyForwardTransform(fftData.data(), true);

        frameEnergy[static_cast<size_t>(frame)] = static_cast<float>(std::sqrt(energy / static_cast<double>(frameSize)));

        float flux = 0.0f;
        for (int bin = 1; bin < halfBins; ++bin)
        {
            const float mag = fftData[static_cast<size_t>(bin)];
            const float diff = juce::jmax(0.0f, mag - prevMag[static_cast<size_t>(bin)]);
            const float weight = 1.0f + (2.0f * static_cast<float>(bin) / static_cast<float>(halfBins));
            flux += diff * weight;
            prevMag[static_cast<size_t>(bin)] = mag;
        }

        spectralFlux[static_cast<size_t>(frame)] = flux;
    }

    std::vector<float> smoothedFlux(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 1);
        const int b = juce::jmin(frames - 1, i + 1);
        float sum = 0.0f;
        for (int k = a; k <= b; ++k)
            sum += spectralFlux[static_cast<size_t>(k)];
        smoothedFlux[static_cast<size_t>(i)] = sum / static_cast<float>(b - a + 1);
    }

    std::vector<float> energyDiff(static_cast<size_t>(frames), 0.0f);
    for (int i = 1; i < frames; ++i)
        energyDiff[static_cast<size_t>(i)] = juce::jmax(0.0f, frameEnergy[static_cast<size_t>(i)] - frameEnergy[static_cast<size_t>(i - 1)]);

    auto medianInWindow = [](const std::vector<float>& values, int start, int end)
    {
        std::vector<float> temp;
        temp.reserve(static_cast<size_t>(end - start + 1));
        for (int i = start; i <= end; ++i)
            temp.push_back(values[static_cast<size_t>(i)]);
        auto midIt = temp.begin() + (temp.size() / 2);
        std::nth_element(temp.begin(), midIt, temp.end());
        return *midIt;
    };

    std::vector<float> novelty(static_cast<size_t>(frames), 0.0f);
    float noveltySum = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 8);
        const int b = juce::jmin(frames - 1, i + 8);
        const float adaptive = (medianInWindow(smoothedFlux, a, b) * 1.25f) + 1.0e-6f;
        const float peakPart = juce::jmax(0.0f, smoothedFlux[static_cast<size_t>(i)] - adaptive);
        const float mixed = peakPart + (0.25f * energyDiff[static_cast<size_t>(i)]);
        novelty[static_cast<size_t>(i)] = mixed;
        noveltySum += mixed;
    }

    const float noveltyMean = noveltySum / static_cast<float>(juce::jmax(1, frames));
    const float noveltyMax = *std::max_element(novelty.begin(), novelty.end());
    const float minPeakLevel = juce::jmax(1.0e-6f,
                                          juce::jmax(noveltyMean * 0.35f, noveltyMax * 0.10f));
    const int minPeakSpacingFrames = juce::jmax(1,
        static_cast<int>((0.015 * sampleRate) / static_cast<double>(hopSize)));

    std::vector<std::pair<int, float>> onsetFrames;
    onsetFrames.reserve(static_cast<size_t>(frames));

    for (int i = 1; i < (frames - 1); ++i)
    {
        const float center = novelty[static_cast<size_t>(i)];
        if (center < minPeakLevel)
            continue;
        if (center < novelty[static_cast<size_t>(i - 1)] || center < novelty[static_cast<size_t>(i + 1)])
            continue;

        if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
        {
            if (center > onsetFrames.back().second)
                onsetFrames.back() = { i, center };
            continue;
        }

        onsetFrames.emplace_back(i, center);
    }

    if (onsetFrames.empty())
    {
        const float energyMax = *std::max_element(energyDiff.begin(), energyDiff.end());
        const float energyMinPeak = juce::jmax(1.0e-6f, energyMax * 0.18f);
        for (int i = 1; i < (frames - 1); ++i)
        {
            const float center = energyDiff[static_cast<size_t>(i)];
            if (center < energyMinPeak)
                continue;
            if (center < energyDiff[static_cast<size_t>(i - 1)] || center < energyDiff[static_cast<size_t>(i + 1)])
                continue;

            if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
                continue;

            onsetFrames.emplace_back(i, center);
        }
    }

    transientSamples.reserve(onsetFrames.size());
    for (const auto& onset : onsetFrames)
    {
        const int centered = (onset.first * hopSize) + (frameSize / 2);
        transientSamples.push_back(static_cast<int64_t>(juce::jlimit(0, totalSamples - 1, centered)));
        if (static_cast<int>(transientSamples.size()) >= kMaxStoredTransientCount)
            break;
    }

    if (const auto refined = refineOnsetSamplesToLeadingEdges(monoBuffer, transientSamples, frameSize, hopSize);
        !refined.empty())
    {
        return refined;
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

double evaluateTempoAutocorrelationScore(const std::vector<int64_t>& transientSamples,
                                         double sampleRate,
                                         double bpm)
{
    if (transientSamples.size() < 2 || sampleRate <= 0.0 || !(bpm > 0.0))
        return 0.0;

    constexpr double pulseRateHz = 200.0;
    const int64_t lastSample = transientSamples.back();
    if (lastSample <= 0)
        return 0.0;

    const int pulseCount = juce::jlimit(64,
                                        200000,
                                        1 + static_cast<int>(std::ceil((static_cast<double>(lastSample) / sampleRate) * pulseRateHz)));
    if (pulseCount <= 4)
        return 0.0;

    std::vector<float> pulseTrain(static_cast<size_t>(pulseCount), 0.0f);
    for (const auto sample : transientSamples)
    {
        const int center = juce::jlimit(0,
                                        pulseCount - 1,
                                        static_cast<int>(std::llround((static_cast<double>(sample) / sampleRate) * pulseRateHz)));
        pulseTrain[static_cast<size_t>(center)] += 1.0f;
        if (center > 0)
            pulseTrain[static_cast<size_t>(center - 1)] += 0.35f;
        if (center + 1 < pulseCount)
            pulseTrain[static_cast<size_t>(center + 1)] += 0.35f;
    }

    const int beatLag = juce::jlimit(1,
                                     pulseCount - 1,
                                     static_cast<int>(std::lround((60.0 / bpm) * pulseRateHz)));
    const int halfLag = juce::jmax(1, beatLag / 2);
    const int doubleLag = juce::jmin(pulseCount - 1, beatLag * 2);

    auto correlationAtLag = [&](int lag)
    {
        if (lag <= 0 || lag >= pulseCount)
            return 0.0;
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i + lag < pulseCount; ++i)
        {
            sum += static_cast<double>(pulseTrain[static_cast<size_t>(i)])
                * static_cast<double>(pulseTrain[static_cast<size_t>(i + lag)]);
            ++count;
        }
        return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
    };

    const double mainScore = correlationAtLag(beatLag);
    const double halfScore = correlationAtLag(halfLag);
    const double doubleScore = correlationAtLag(doubleLag);
    return mainScore + (0.35 * doubleScore) + (0.15 * halfScore);
}

double estimateTempoFromTransients(const std::vector<int64_t>& transientSamples, double sampleRate)
{
    if (transientSamples.size() < 2 || sampleRate <= 0.0)
        return 0.0;

    constexpr double bpmMin = 70.0;
    constexpr double bpmMax = 180.0;
    constexpr double bpmStep = 0.5;
    constexpr int histogramBins = static_cast<int>(((bpmMax - bpmMin) / bpmStep) + 1.0);
    std::array<double, histogramBins> histogram {};
    std::array<double, histogramBins> autoCorrelationScores {};

    auto addCandidate = [&](double bpm, double weight)
    {
        bpm = normalizeTempoEstimate(bpm);
        if (!(bpm >= bpmMin && bpm <= bpmMax) || !std::isfinite(weight) || weight <= 0.0)
            return;

        const int index = juce::jlimit(0,
                                       histogramBins - 1,
                                       static_cast<int>(std::lround((bpm - bpmMin) / bpmStep)));
        histogram[static_cast<size_t>(index)] += weight;
    };

    for (size_t i = 0; i + 1 < transientSamples.size(); ++i)
    {
        const size_t maxNeighbor = juce::jmin(transientSamples.size(), i + 12);
        for (size_t j = i + 1; j < maxNeighbor; ++j)
        {
            const double deltaSamples = static_cast<double>(transientSamples[j] - transientSamples[i]);
            const double seconds = deltaSamples / sampleRate;
            if (seconds <= 0.08 || seconds >= 2.5)
                continue;

            const double distanceWeight = 1.0 / static_cast<double>(j - i);
            for (int beatMultiple = 1; beatMultiple <= 8; ++beatMultiple)
            {
                const double bpm = (60.0 * static_cast<double>(beatMultiple)) / seconds;
                const double multipleWeight = distanceWeight / std::sqrt(static_cast<double>(beatMultiple));
                addCandidate(bpm, multipleWeight);
            }
        }
    }

    std::array<double, histogramBins> smoothed {};
    for (int i = 0; i < histogramBins; ++i)
    {
        const double left = histogram[static_cast<size_t>(juce::jmax(0, i - 1))];
        const double center = histogram[static_cast<size_t>(i)];
        const double right = histogram[static_cast<size_t>(juce::jmin(histogramBins - 1, i + 1))];
        smoothed[static_cast<size_t>(i)] = (left * 0.25) + (center * 0.5) + (right * 0.25);
    }

    const double histogramPeak = *std::max_element(smoothed.begin(), smoothed.end());
    if (!(histogramPeak > 0.0))
        return 0.0;

    double autoPeak = 0.0;
    for (int i = 0; i < histogramBins; ++i)
    {
        const double bpm = bpmMin + (static_cast<double>(i) * bpmStep);
        autoCorrelationScores[static_cast<size_t>(i)] = evaluateTempoAutocorrelationScore(transientSamples, sampleRate, bpm);
        autoPeak = juce::jmax(autoPeak, autoCorrelationScores[static_cast<size_t>(i)]);
    }

    std::array<double, histogramBins> combined {};
    for (int i = 0; i < histogramBins; ++i)
    {
        const double bpm = bpmMin + (static_cast<double>(i) * bpmStep);
        const double primary = smoothed[static_cast<size_t>(i)] / histogramPeak;
        const double autoScore = autoPeak > 0.0
            ? (autoCorrelationScores[static_cast<size_t>(i)] / autoPeak)
            : 0.0;

        double harmonicScore = primary;
        auto sampleNormalized = [&](double candidateBpm, double weight)
        {
            if (!(candidateBpm >= bpmMin && candidateBpm <= bpmMax))
                return 0.0;
            const int idx = juce::jlimit(0,
                                         histogramBins - 1,
                                         static_cast<int>(std::lround((candidateBpm - bpmMin) / bpmStep)));
            return (smoothed[static_cast<size_t>(idx)] / histogramPeak) * weight;
        };

        harmonicScore += sampleNormalized(normalizeTempoEstimate(bpm * 2.0), 0.28);
        harmonicScore += sampleNormalized(normalizeTempoEstimate(bpm * 0.5), 0.18);

        combined[static_cast<size_t>(i)] = (harmonicScore * 0.65) + (autoScore * 0.35);
    }

    const auto bestIt = std::max_element(combined.begin(), combined.end());
    if (bestIt == combined.end() || *bestIt <= 0.0)
        return 0.0;

    const int bestIndex = static_cast<int>(std::distance(combined.begin(), bestIt));
    return bpmMin + (static_cast<double>(bestIndex) * bpmStep);
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

#if !MLRVST_ENABLE_ESSENTIA_NATIVE
juce::String buildEssentiaScript()
{
    return juce::String(
R"PY(import json
import math
import os
import sys
import tempfile

os.environ.setdefault("MPLCONFIGDIR", os.path.join(tempfile.gettempdir(), "mlrvst_essentia_mpl"))
os.environ.setdefault("XDG_CACHE_HOME", tempfile.gettempdir())

try:
    import numpy as np
    import essentia
    import essentia.standard as es
except Exception as exc:
    print(json.dumps({"ok": False, "error": str(exc)}))
    raise SystemExit(0)

path = sys.argv[1]

try:
    sr = 44100
    audio_arr = es.MonoLoader(filename=path, sampleRate=sr)()
    audio_arr = np.asarray(audio_arr, dtype=np.float32).reshape(-1)

    if audio_arr.size == 0:
        raise RuntimeError("empty audio")

    rhythm = es.RhythmExtractor2013(method="multifeature")
    bpm, beats, beats_confidence, _, _ = rhythm(audio_arr)
    beats = np.asarray(beats, dtype=np.float32).reshape(-1)
    transient_samples = [int(round(float(t) * sr)) for t in beats[:512]]
    tempo = float(bpm) if np.isfinite(bpm) else 0.0

    print(json.dumps({
        "ok": True,
        "tempo_bpm": tempo,
        "pitch_hz": 0.0,
        "pitch_midi": -1,
        "transient_samples": transient_samples,
        "analysis_source": "essentia"
    }))
except Exception as exc:
    print(json.dumps({"ok": False, "error": str(exc)}))
)PY");
}

juce::File ensureEssentiaScriptFile()
{
    const auto scriptFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile(kEssentiaScriptName);
    const auto scriptText = buildEssentiaScript();

    if (!scriptFile.existsAsFile() || scriptFile.loadFileAsString() != scriptText)
        scriptFile.replaceWithText(scriptText);

    return scriptFile;
}

std::optional<juce::File> createEssentiaAnalysisWaveFile(const LoadedSampleData& sampleData,
                                                         juce::String* failureReason = nullptr)
{
    if (sampleData.audioBuffer.getNumSamples() <= 0 || sampleData.audioBuffer.getNumChannels() <= 0)
    {
        if (failureReason != nullptr)
            *failureReason = "ES empty analysis buffer";
        return std::nullopt;
    }

    juce::TemporaryFile tempFile("wav");
    const auto tempPath = tempFile.getFile();

    juce::WavAudioFormat wavFormat;
    auto outputStream = std::unique_ptr<juce::FileOutputStream>(tempPath.createOutputStream());
    if (outputStream == nullptr)
    {
        if (failureReason != nullptr)
            *failureReason = "ES temp file open failed";
        return std::nullopt;
    }

    auto* rawStream = outputStream.release();
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(rawStream,
                                                                              sampleData.sourceSampleRate,
                                                                              static_cast<unsigned int>(sampleData.audioBuffer.getNumChannels()),
                                                                              24,
                                                                              {},
                                                                              0));
    if (writer == nullptr)
    {
        delete rawStream;
        if (failureReason != nullptr)
            *failureReason = "ES temp writer failed";
        return std::nullopt;
    }

    if (!writer->writeFromAudioSampleBuffer(sampleData.audioBuffer, 0, sampleData.audioBuffer.getNumSamples()))
    {
        writer.reset();
        tempPath.deleteFile();
        if (failureReason != nullptr)
            *failureReason = "ES temp write failed";
        return std::nullopt;
    }

    writer.reset();
    return tempPath;
}

std::optional<SampleAnalysisSummary> runEssentiaAnalysis(const juce::File& sourceFile,
                                                         juce::String* failureReason = nullptr)
{
    if (!sourceFile.existsAsFile())
    {
        if (failureReason != nullptr)
            *failureReason = "ES source file missing";
        return std::nullopt;
    }

    const auto scriptFile = ensureEssentiaScriptFile();
    if (!scriptFile.existsAsFile())
    {
        if (failureReason != nullptr)
            *failureReason = "ES script missing";
        return std::nullopt;
    }

    const auto& pythonConfig = getEssentiaPythonConfig();
    if (!pythonConfig.valid)
    {
        if (failureReason != nullptr)
            *failureReason = pythonConfig.failureReason;
        return std::nullopt;
    }

    juce::ChildProcess childProcess;
    juce::StringArray args;
    args.add("/usr/bin/env");
    args.add("HOME=" + juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName());
    args.add("PATH=" + pythonConfig.pathEnv);
    if (pythonConfig.pythonPath.isNotEmpty())
        args.add("PYTHONPATH=" + pythonConfig.pythonPath);
    args.add("MPLCONFIGDIR=" + juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("mlrvst_essentia_mpl")
                                   .getFullPathName());
    args.add("XDG_CACHE_HOME=" + juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName());
    args.add(pythonConfig.executable);
    args.add(scriptFile.getFullPathName());
    args.add(sourceFile.getFullPathName());

    if (!childProcess.start(args))
    {
        if (failureReason != nullptr)
            *failureReason = "ES worker launch failed";
        return std::nullopt;
    }

    if (!childProcess.waitForProcessToFinish(30000))
    {
        childProcess.kill();
        if (failureReason != nullptr)
            *failureReason = "ES timeout";
        return std::nullopt;
    }

    const auto output = childProcess.readAllProcessOutput().trim();
    if (output.isEmpty())
    {
        if (failureReason != nullptr)
            *failureReason = "ES empty output";
        return std::nullopt;
    }

    juce::var parsed;
    {
        const auto lines = juce::StringArray::fromLines(output);
        for (int i = lines.size() - 1; i >= 0; --i)
        {
            const auto candidate = lines[i].trim();
            if (!candidate.startsWithChar('{'))
                continue;

            parsed = juce::JSON::parse(candidate);
            if (!parsed.isVoid())
                break;
        }
    }

    if (parsed.isVoid())
    {
        if (failureReason != nullptr)
            *failureReason = sanitizeEssentiaFailureReason(output);
        parsed = juce::JSON::parse(output);
    }

    if (!parsed.isObject())
    {
        if (failureReason != nullptr && failureReason->isEmpty())
            *failureReason = "ES invalid JSON";
        return std::nullopt;
    }

    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
    {
        if (failureReason != nullptr)
            *failureReason = "ES invalid result";
        return std::nullopt;
    }

    if (!static_cast<bool>(object->getProperty("ok")))
    {
        if (failureReason != nullptr)
            *failureReason = sanitizeEssentiaFailureReason(object->getProperty("error").toString());
        return std::nullopt;
    }

    SampleAnalysisSummary summary;
    summary.estimatedTempoBpm = normalizeTempoEstimate(static_cast<double>(object->getProperty("tempo_bpm")));
    summary.estimatedPitchHz = static_cast<double>(object->getProperty("pitch_hz"));
    summary.estimatedPitchMidi = static_cast<int>(object->getProperty("pitch_midi"));
    summary.essentiaUsed = true;
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
#else
struct EssentiaNativeRuntime
{
    EssentiaNativeRuntime() { essentia::init(); }
    ~EssentiaNativeRuntime() { essentia::shutdown(); }
};

EssentiaNativeRuntime& getEssentiaNativeRuntime()
{
    static EssentiaNativeRuntime runtime;
    return runtime;
}

std::optional<SampleAnalysisSummary> runEssentiaAnalysis(const LoadedSampleData& sampleData,
                                                         juce::String* failureReason = nullptr)
{
    if (sampleData.audioBuffer.getNumSamples() <= 0 || sampleData.sourceSampleRate <= 0.0)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native empty buffer";
        return std::nullopt;
    }

    juce::ignoreUnused(getEssentiaNativeRuntime());

    const auto monoBuffer = buildMonoBuffer(sampleData);
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 0)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native empty mono";
        return std::nullopt;
    }

    std::vector<essentia::Real> signal(static_cast<size_t>(totalSamples));
    const auto* mono = monoBuffer.getReadPointer(0);
    for (int i = 0; i < totalSamples; ++i)
        signal[static_cast<size_t>(i)] = mono[i];

    SampleAnalysisSummary summary;

    try
    {
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        std::unique_ptr<essentia::standard::Algorithm> rhythm(
            factory.create("RhythmExtractor2013",
                           "method", "multifeature",
                           "minTempo", 40.0,
                           "maxTempo", 240.0));

        essentia::Real bpm = 0.0f;
        essentia::Real confidence = 0.0f;
        std::vector<essentia::Real> ticks;
        std::vector<essentia::Real> estimates;
        std::vector<essentia::Real> bpmIntervals;
        rhythm->input("signal").set(signal);
        rhythm->output("bpm").set(bpm);
        rhythm->output("ticks").set(ticks);
        rhythm->output("confidence").set(confidence);
        rhythm->output("estimates").set(estimates);
        rhythm->output("bpmIntervals").set(bpmIntervals);
        rhythm->compute();

        summary.estimatedTempoBpm = normalizeTempoEstimate(static_cast<double>(bpm));
        summary.beatTickSamples.reserve(ticks.size());
        for (const auto tickSeconds : ticks)
        {
            if (!std::isfinite(tickSeconds))
                continue;
            const int64_t tickSample = static_cast<int64_t>(std::llround(
                static_cast<double>(tickSeconds) * sampleData.sourceSampleRate));
            summary.beatTickSamples.push_back(
                juce::jlimit<int64_t>(0, juce::jmax<int64_t>(0, sampleData.sourceLengthSamples - 1), tickSample));
            if (static_cast<int>(summary.beatTickSamples.size()) >= kMaxStoredTransientCount)
                break;
        }

        int frameSize = 2048;
        while (frameSize > totalSamples && frameSize > 256)
            frameSize /= 2;
        const int hopSize = juce::jmax(64, frameSize / 8);

        std::unique_ptr<essentia::standard::Algorithm> frameCutter(
            factory.create("FrameCutter",
                           "frameSize", frameSize,
                           "hopSize", hopSize,
                           "startFromZero", false));
        std::unique_ptr<essentia::standard::Algorithm> window(
            factory.create("Windowing", "type", "hann", "zeroPadding", 0));
        std::unique_ptr<essentia::standard::Algorithm> spectrum(
            factory.create("Spectrum", "size", frameSize));
        std::unique_ptr<essentia::standard::Algorithm> pitchDetect(
            factory.create("PitchYinFFT",
                           "frameSize", frameSize,
                           "sampleRate", sampleData.sourceSampleRate));

        std::vector<essentia::Real> frame;
        std::vector<essentia::Real> windowedFrame;
        std::vector<essentia::Real> magnitudeSpectrum;
        essentia::Real pitchHz = 0.0f;
        essentia::Real pitchConfidence = 0.0f;
        frameCutter->input("signal").set(signal);
        frameCutter->output("frame").set(frame);
        window->input("frame").set(frame);
        window->output("frame").set(windowedFrame);
        spectrum->input("frame").set(windowedFrame);
        spectrum->output("spectrum").set(magnitudeSpectrum);
        pitchDetect->input("spectrum").set(magnitudeSpectrum);
        pitchDetect->output("pitch").set(pitchHz);
        pitchDetect->output("pitchConfidence").set(pitchConfidence);

        std::vector<double> detectedPitchesHz;
        while (true)
        {
            frameCutter->compute();
            if (frame.empty())
                break;

            float peak = 0.0f;
            for (const auto sample : frame)
                peak = juce::jmax(peak, std::abs(sample));
            if (peak < 1.0e-4f)
                continue;

            window->compute();
            spectrum->compute();
            pitchDetect->compute();

            if (std::isfinite(pitchHz) && pitchHz > 20.0f && pitchConfidence >= 0.45f)
                detectedPitchesHz.push_back(static_cast<double>(pitchHz));
        }

        if (!detectedPitchesHz.empty())
        {
            std::sort(detectedPitchesHz.begin(), detectedPitchesHz.end());
            const size_t medianIndex = detectedPitchesHz.size() / 2;
            summary.estimatedPitchHz = detectedPitchesHz[medianIndex];
            summary.estimatedPitchMidi = pitchHzToMidi(summary.estimatedPitchHz);
        }

        const bool hasTempo = summary.estimatedTempoBpm > 0.0 || !summary.beatTickSamples.empty();
        const bool hasPitch = summary.estimatedPitchMidi >= 0;
        if (!hasTempo && !hasPitch)
        {
            if (failureReason != nullptr)
                *failureReason = "ES native no descriptors";
            return std::nullopt;
        }

        summary.essentiaUsed = true;
        summary.analysisSource = hasPitch
            ? "essentia native tempo/pitch/ticks"
            : "essentia native tempo/ticks";
        return summary;
    }
    catch (const std::exception& exception)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native " + juce::String(exception.what()).substring(0, 96);
    }
    catch (...)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native exception";
    }

    return std::nullopt;
}
#endif

SampleAnalysisSummary buildInternalAnalysis(const LoadedSampleData& sampleData)
{
    SampleAnalysisSummary summary;
    summary.analysisSource = "internal";

    const auto monoBuffer = buildMonoBuffer(sampleData);
    summary.transientSamples = detectLoopStyleTransientSamples(monoBuffer,
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

std::shared_ptr<const StretchedSliceBuffer> buildStretchedSliceBuffer(const LoadedSampleData& sampleData,
                                                                      const SampleSlice& slice,
                                                                      float playbackRate,
                                                                      float pitchSemitones,
                                                                      TimeStretchBackend backend)
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

    juce::AudioBuffer<float> stretchedBuffer;
    if (!renderTimeStretchedBufferForRate(sourceSlice,
                                          sampleData.sourceSampleRate,
                                          quantizedRate,
                                          quantizedPitch,
                                          backend,
                                          stretchedBuffer))
        return {};

    const int outputFrames = stretchedBuffer.getNumSamples();
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
        cacheLeft[frame] = stretchedBuffer.getSample(0, frame);
        cacheRight[frame] = stretchedBuffer.getSample(1, frame);
    }

    return std::const_pointer_cast<const StretchedSliceBuffer>(cache);
}

juce::String buildSliceLabel(int index)
{
    return "S" + juce::String(index + 1);
}

juce::String legacyLoopBarSelectionLabel(int selection)
{
    switch (selection)
    {
        case 25:  return "1/4";
        case 50:  return "1/2";
        case 100: return "1B";
        case 200: return "2B";
        case 400: return "4B";
        case 800: return "8B";
        default:  return "AUTO";
    }
}

float legacyLoopBarSelectionToBeats(int selection)
{
    switch (selection)
    {
        case 25:  return 1.0f;
        case 50:  return 2.0f;
        case 100: return 4.0f;
        case 200: return 8.0f;
        case 400: return 16.0f;
        case 800: return 32.0f;
        default:  return 0.0f;
    }
}

std::vector<int64_t> sanitizeBeatTickSamples(const std::vector<int64_t>& beatTickSamples,
                                             int64_t totalSamples)
{
    std::vector<int64_t> sanitized;
    sanitized.reserve(beatTickSamples.size());

    for (const auto sample : beatTickSamples)
    {
        if (sample < 0 || sample >= totalSamples)
            continue;
        if (!sanitized.empty() && sample <= sanitized.back())
            continue;
        sanitized.push_back(sample);
    }

    return sanitized;
}

int64_t computeMedianBeatTickInterval(const std::vector<int64_t>& beatTicks)
{
    if (beatTicks.size() < 2)
        return 0;

    std::vector<int64_t> intervals;
    intervals.reserve(beatTicks.size() - 1);
    for (size_t i = 1; i < beatTicks.size(); ++i)
    {
        const int64_t interval = beatTicks[i] - beatTicks[i - 1];
        if (interval > 0)
            intervals.push_back(interval);
    }

    if (intervals.empty())
        return 0;

    std::sort(intervals.begin(), intervals.end());
    return intervals[intervals.size() / 2];
}

double computeMedianDouble(std::vector<double> values)
{
    if (values.empty())
        return 0.0;

    std::sort(values.begin(), values.end());
    const size_t mid = values.size() / 2;
    if ((values.size() & 1u) == 0u)
        return 0.5 * (values[mid - 1] + values[mid]);
    return values[mid];
}

std::vector<int64_t> buildWholeFileDriftCorrectedBeatTicks(const std::vector<int64_t>& rawBeatTicks,
                                                           const std::vector<int64_t>& transientSamples,
                                                           int64_t totalSamples)
{
    auto corrected = sanitizeBeatTickSamples(rawBeatTicks, totalSamples);
    if (corrected.size() < 2 || totalSamples <= 0)
        return corrected;

    const int64_t medianInterval = computeMedianBeatTickInterval(corrected);
    if (medianInterval <= 0)
        return corrected;

    std::vector<double> interceptCandidates;
    interceptCandidates.reserve(corrected.size());
    for (size_t i = 0; i < corrected.size(); ++i)
        interceptCandidates.push_back(static_cast<double>(corrected[i])
                                      - (static_cast<double>(i) * static_cast<double>(medianInterval)));

    const double intercept = computeMedianDouble(std::move(interceptCandidates));
    const auto snappedTransients = sanitizeBeatTickSamples(transientSamples, totalSamples);
    const int64_t maxSnapDistance = juce::jmax<int64_t>(1, static_cast<int64_t>(std::llround(static_cast<double>(medianInterval) * 0.18)));

    std::vector<int64_t> stabilized;
    stabilized.reserve(corrected.size());
    int64_t previous = -1;

    for (size_t i = 0; i < corrected.size(); ++i)
    {
        int64_t chosen = static_cast<int64_t>(std::llround(
            intercept + (static_cast<double>(i) * static_cast<double>(medianInterval))));
        chosen = juce::jlimit<int64_t>(0, juce::jmax<int64_t>(0, totalSamples - 1), chosen);

        if (!snappedTransients.empty())
        {
            auto it = std::lower_bound(snappedTransients.begin(), snappedTransients.end(), chosen);
            int64_t bestCandidate = chosen;
            int64_t bestDistance = std::numeric_limits<int64_t>::max();

            if (it != snappedTransients.end())
            {
                const int64_t distance = std::abs(*it - chosen);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestCandidate = *it;
                }
            }
            if (it != snappedTransients.begin())
            {
                const int64_t candidate = *std::prev(it);
                const int64_t distance = std::abs(candidate - chosen);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestCandidate = candidate;
                }
            }

            if (bestDistance <= maxSnapDistance)
                chosen = bestCandidate;
        }

        chosen = juce::jlimit<int64_t>(juce::jmax<int64_t>(0, previous + 1),
                                       juce::jmax<int64_t>(0, totalSamples - 1),
                                       chosen);
        stabilized.push_back(chosen);
        previous = chosen;
    }

    return sanitizeBeatTickSamples(stabilized, totalSamples);
}

int findNearestBeatTickIndex(const std::vector<int64_t>& beatTicks, int64_t targetSample)
{
    if (beatTicks.empty())
        return -1;

    auto it = std::lower_bound(beatTicks.begin(), beatTicks.end(), targetSample);
    if (it == beatTicks.begin())
        return 0;
    if (it == beatTicks.end())
        return static_cast<int>(beatTicks.size()) - 1;

    const int nextIndex = static_cast<int>(std::distance(beatTicks.begin(), it));
    const int prevIndex = nextIndex - 1;
    const int64_t nextDistance = std::abs(*it - targetSample);
    const int64_t prevDistance = std::abs(beatTicks[static_cast<size_t>(prevIndex)] - targetSample);
    return (prevDistance <= nextDistance) ? prevIndex : nextIndex;
}

bool computeBeatTickAlignedWindowRange(const std::vector<int64_t>& beatTicks,
                                       int64_t sourceLengthSamples,
                                       int64_t desiredStartSample,
                                       int beatCount,
                                       int64_t& rangeStartSample,
                                       int64_t& rangeEndSample)
{
    rangeStartSample = 0;
    rangeEndSample = 0;

    if (beatCount <= 0 || sourceLengthSamples <= 0 || beatTicks.size() < 2)
        return false;

    const int startIndex = findNearestBeatTickIndex(beatTicks, desiredStartSample);
    if (startIndex < 0)
        return false;

    const int64_t medianInterval = computeMedianBeatTickInterval(beatTicks);
    if (medianInterval <= 0)
        return false;

    const int endIndex = startIndex + beatCount;
    rangeStartSample = beatTicks[static_cast<size_t>(startIndex)];

    if (endIndex < static_cast<int>(beatTicks.size()))
    {
        rangeEndSample = beatTicks[static_cast<size_t>(endIndex)];
    }
    else
    {
        const int extrapolatedBeats = endIndex - (static_cast<int>(beatTicks.size()) - 1);
        rangeEndSample = beatTicks.back() + (static_cast<int64_t>(extrapolatedBeats) * medianInterval);
    }

    rangeStartSample = juce::jlimit<int64_t>(0, sourceLengthSamples - 1, rangeStartSample);
    rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1,
                                           sourceLengthSamples,
                                           rangeEndSample);
    return rangeEndSample > rangeStartSample;
}

double computeBeatTickPosition(const std::vector<int64_t>& beatTicks,
                               int64_t samplePosition)
{
    if (beatTicks.size() < 2)
        return 0.0;

    const int64_t medianInterval = computeMedianBeatTickInterval(beatTicks);
    if (medianInterval <= 0)
        return 0.0;

    auto it = std::lower_bound(beatTicks.begin(), beatTicks.end(), samplePosition);
    if (it == beatTicks.begin())
        return static_cast<double>(samplePosition - beatTicks.front()) / static_cast<double>(medianInterval);

    if (it == beatTicks.end())
    {
        const double beatsBeyondEnd = static_cast<double>(samplePosition - beatTicks.back())
            / static_cast<double>(medianInterval);
        return static_cast<double>(beatTicks.size() - 1) + beatsBeyondEnd;
    }

    const int nextIndex = static_cast<int>(std::distance(beatTicks.begin(), it));
    const int prevIndex = nextIndex - 1;
    const int64_t start = beatTicks[static_cast<size_t>(prevIndex)];
    const int64_t end = beatTicks[static_cast<size_t>(nextIndex)];
    const int64_t interval = juce::jmax<int64_t>(1, end - start);
    const double alpha = static_cast<double>(samplePosition - start) / static_cast<double>(interval);
    return static_cast<double>(prevIndex) + juce::jlimit(0.0, 1.0, alpha);
}

int64_t samplePositionFromBeatTickPosition(const std::vector<int64_t>& beatTicks,
                                           double beatPosition,
                                           int64_t sourceLengthSamples)
{
    if (beatTicks.empty() || sourceLengthSamples <= 0 || !std::isfinite(beatPosition))
        return 0;

    const int64_t medianInterval = computeMedianBeatTickInterval(beatTicks);
    if (medianInterval <= 0)
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, beatTicks.front());

    if (beatPosition <= 0.0)
    {
        const double sample = static_cast<double>(beatTicks.front())
            + (beatPosition * static_cast<double>(medianInterval));
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int maxIndex = static_cast<int>(beatTicks.size()) - 1;
    if (beatPosition >= static_cast<double>(maxIndex))
    {
        const double sample = static_cast<double>(beatTicks.back())
            + ((beatPosition - static_cast<double>(maxIndex)) * static_cast<double>(medianInterval));
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int lowerIndex = juce::jlimit(0, maxIndex - 1, static_cast<int>(std::floor(beatPosition)));
    const int upperIndex = juce::jlimit(lowerIndex + 1, maxIndex, lowerIndex + 1);
    const double alpha = juce::jlimit(0.0, 1.0, beatPosition - static_cast<double>(lowerIndex));
    const double start = static_cast<double>(beatTicks[static_cast<size_t>(lowerIndex)]);
    const double end = static_cast<double>(beatTicks[static_cast<size_t>(upperIndex)]);
    return juce::jlimit<int64_t>(0,
                                 sourceLengthSamples - 1,
                                 static_cast<int64_t>(std::llround(start + ((end - start) * alpha))));
}

float computeBeatSpanFromBeatTicks(const std::vector<int64_t>& beatTicks,
                                   int64_t rangeStartSample,
                                   int64_t rangeEndSample)
{
    if (beatTicks.size() < 2 || rangeEndSample <= rangeStartSample)
        return 0.0f;

    const double startBeat = computeBeatTickPosition(beatTicks, rangeStartSample);
    const double endBeat = computeBeatTickPosition(beatTicks, rangeEndSample);
    const double span = endBeat - startBeat;
    if (!(span > 0.0) || !std::isfinite(span))
        return 0.0f;

    return static_cast<float>(span);
}

float computeVisibleSliceBeatSpan(const LoadedSampleData& sampleData,
                                  const std::array<SampleSlice, SliceModel::VisibleSliceCount>& visibleSlices,
                                  double fallbackTempoBpm,
                                  int legacyLoopBarSelection)
{
    if (legacyLoopBarSelection > 0)
        return legacyLoopBarSelectionToBeats(legacyLoopBarSelection);

    int64_t rangeStartSample = std::numeric_limits<int64_t>::max();
    int64_t rangeEndSample = 0;
    for (const auto& slice : visibleSlices)
    {
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;
        rangeStartSample = juce::jmin(rangeStartSample, slice.startSample);
        rangeEndSample = juce::jmax(rangeEndSample, slice.endSample);
    }

    if (rangeStartSample == std::numeric_limits<int64_t>::max() || rangeEndSample <= rangeStartSample)
        return 0.0f;

    const auto beatTicks = sanitizeBeatTickSamples(sampleData.analysis.beatTickSamples,
                                                   sampleData.sourceLengthSamples);
    if (const auto tickBeatSpan = computeBeatSpanFromBeatTicks(beatTicks, rangeStartSample, rangeEndSample);
        tickBeatSpan > 0.0f)
    {
        return tickBeatSpan;
    }

    if (fallbackTempoBpm > 0.0 && std::isfinite(fallbackTempoBpm) && sampleData.sourceSampleRate > 0.0)
    {
        const double seconds = static_cast<double>(rangeEndSample - rangeStartSample)
            / sampleData.sourceSampleRate;
        const double beats = seconds * (fallbackTempoBpm / 60.0);
        if (beats > 0.0 && std::isfinite(beats))
            return static_cast<float>(beats);
    }

    return 0.0f;
}

float normalizedPositionFromSample(int64_t samplePosition, int64_t totalSamples)
{
    return safeNormalizedPosition(samplePosition, totalSamples);
}

std::optional<juce::Range<float>> computeSampleLoopVisualRange(const SampleModeEngine::StateSnapshot& snapshot)
{
    if (!snapshot.hasSample || snapshot.totalSamples <= 0)
        return std::nullopt;

    if (snapshot.useLegacyLoopEngine || snapshot.legacyLoopBarSelection > 0)
    {
        int64_t rangeStart = std::numeric_limits<int64_t>::max();
        int64_t rangeEnd = 0;
        for (const auto& slice : snapshot.visibleSlices)
        {
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            rangeStart = juce::jmin(rangeStart, slice.startSample);
            rangeEnd = juce::jmax(rangeEnd, slice.endSample);
        }

        if (rangeStart == std::numeric_limits<int64_t>::max() || rangeEnd <= rangeStart)
            return std::nullopt;

        if (snapshot.legacyLoopBarSelection > 0
            && snapshot.sourceSampleRate > 0.0
            && snapshot.estimatedTempoBpm > 0.0)
        {
            float beatsForLoop = 0.0f;
            switch (snapshot.legacyLoopBarSelection)
            {
                case 25:  beatsForLoop = 1.0f; break;
                case 50:  beatsForLoop = 2.0f; break;
                case 100: beatsForLoop = 4.0f; break;
                case 200: beatsForLoop = 8.0f; break;
                case 400: beatsForLoop = 16.0f; break;
                case 800: beatsForLoop = 32.0f; break;
                default: break;
            }

            if (beatsForLoop > 0.0f)
            {
                const double samplesPerBeat = (60.0 / snapshot.estimatedTempoBpm) * snapshot.sourceSampleRate;
                const int64_t targetLength = static_cast<int64_t>(std::llround(static_cast<double>(beatsForLoop) * samplesPerBeat));
                if (targetLength > 0)
                    rangeEnd = juce::jlimit<int64_t>(rangeStart + 1, snapshot.totalSamples, rangeStart + targetLength);
            }
        }

        return juce::Range<float>(normalizedPositionFromSample(rangeStart, snapshot.totalSamples),
                                  normalizedPositionFromSample(rangeEnd, snapshot.totalSamples));
    }

    if (snapshot.triggerMode != SampleTriggerMode::Loop)
        return std::nullopt;

    int slot = snapshot.activeVisibleSliceSlot;
    if (slot < 0)
        slot = snapshot.pendingVisibleSliceSlot;
    if (slot < 0 && snapshot.playbackProgress >= 0.0f)
    {
        for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
        {
            const auto& slice = snapshot.visibleSlices[static_cast<size_t>(i)];
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            const bool isLastSlice = (i == (SliceModel::VisibleSliceCount - 1));
            if (snapshot.playbackProgress >= slice.normalizedStart
                && (snapshot.playbackProgress < slice.normalizedEnd
                    || (isLastSlice && snapshot.playbackProgress <= slice.normalizedEnd)))
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0 || slot >= SliceModel::VisibleSliceCount)
        return std::nullopt;

    const auto& slice = snapshot.visibleSlices[static_cast<size_t>(slot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return std::nullopt;

    return juce::Range<float>(slice.normalizedStart, slice.normalizedEnd);
}
} // namespace

juce::ValueTree SampleModePersistentState::createValueTree(const juce::Identifier& type) const
{
    juce::ValueTree tree(type);
    tree.setProperty("samplePath", samplePath, nullptr);
    tree.setProperty("visibleSliceBankIndex", visibleSliceBankIndex, nullptr);
    tree.setProperty("sliceMode", sampleSliceModeName(sliceMode), nullptr);
    tree.setProperty("triggerMode", sampleTriggerModeName(triggerMode), nullptr);
    tree.setProperty("useLegacyLoopEngine", useLegacyLoopEngine, nullptr);
    tree.setProperty("legacyLoopBarSelection", legacyLoopBarSelection, nullptr);
    tree.setProperty("beatDivision", beatDivision, nullptr);
    tree.setProperty("viewZoom", viewZoom, nullptr);
    tree.setProperty("viewScroll", viewScroll, nullptr);
    tree.setProperty("selectedCueIndex", selectedCueIndex, nullptr);
    tree.setProperty("analyzedTempoBpm", analyzedTempoBpm, nullptr);
    tree.setProperty("analyzedPitchHz", analyzedPitchHz, nullptr);
    tree.setProperty("analyzedPitchMidi", analyzedPitchMidi, nullptr);
    tree.setProperty("essentiaUsed", essentiaUsed, nullptr);
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

    juce::ValueTree transientNode("TransientMarkers");
    for (const auto sample : transientEditSamples)
    {
        juce::ValueTree markerNode("Marker");
        markerNode.setProperty("samplePosition", static_cast<juce::int64>(sample), nullptr);
        transientNode.addChild(markerNode, -1, nullptr);
    }
    tree.addChild(transientNode, -1, nullptr);

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
    persistentState.useLegacyLoopEngine = static_cast<bool>(state.getProperty("useLegacyLoopEngine", false));
    persistentState.legacyLoopBarSelection = static_cast<int>(state.getProperty("legacyLoopBarSelection", 0));
    persistentState.beatDivision = juce::jlimit(1, 8, static_cast<int>(state.getProperty("beatDivision", 1)));
    persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, static_cast<float>(state.getProperty("viewZoom", 1.0f)));
    persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, static_cast<float>(state.getProperty("viewScroll", 0.0f)));
    persistentState.selectedCueIndex = static_cast<int>(state.getProperty("selectedCueIndex", -1));
    persistentState.analyzedTempoBpm = static_cast<double>(state.getProperty("analyzedTempoBpm", 0.0));
    persistentState.analyzedPitchHz = static_cast<double>(state.getProperty("analyzedPitchHz", 0.0));
    persistentState.analyzedPitchMidi = static_cast<int>(state.getProperty("analyzedPitchMidi", -1));
    persistentState.essentiaUsed = static_cast<bool>(state.getProperty("essentiaUsed", false));
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

    if (auto transientNode = state.getChildWithName("TransientMarkers"); transientNode.isValid())
    {
        for (auto markerNode : transientNode)
        {
            if (!markerNode.hasType("Marker"))
                continue;

            persistentState.transientEditSamples.push_back(
                static_cast<int64_t>(static_cast<juce::int64>(markerNode.getProperty("samplePosition", 0))));
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
    xml->setAttribute("useLegacyLoopEngine", useLegacyLoopEngine);
    xml->setAttribute("legacyLoopBarSelection", legacyLoopBarSelection);
    xml->setAttribute("beatDivision", beatDivision);
    xml->setAttribute("viewZoom", viewZoom);
    xml->setAttribute("viewScroll", viewScroll);
    xml->setAttribute("selectedCueIndex", selectedCueIndex);
    xml->setAttribute("analyzedTempoBpm", analyzedTempoBpm);
    xml->setAttribute("analyzedPitchHz", analyzedPitchHz);
    xml->setAttribute("analyzedPitchMidi", analyzedPitchMidi);
    xml->setAttribute("essentiaUsed", essentiaUsed);
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

    auto* transientXml = xml->createNewChildElement("TransientMarkers");
    for (const auto marker : transientEditSamples)
    {
        auto* markerXml = transientXml->createNewChildElement("Marker");
        markerXml->setAttribute("samplePosition", juce::String(static_cast<juce::int64>(marker)));
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
                                                     int beatDivision,
                                                     const std::vector<int64_t>& beatTickSamples)
{
    if (sampleData.sourceLengthSamples <= 0 || sampleData.sourceSampleRate <= 0.0 || !(tempoBpm > 0.0))
        return {};

    const int safeBeatDivision = juce::jlimit(1, 8, beatDivision);
    std::vector<SampleSlice> slices;

    if (beatTickSamples.size() >= 2)
    {
        std::vector<int64_t> anchors;
        anchors.reserve(beatTickSamples.size() + 4);
        for (const auto sample : beatTickSamples)
        {
            if (sample < 0 || sample >= sampleData.sourceLengthSamples)
                continue;
            if (!anchors.empty() && sample <= anchors.back())
                continue;
            anchors.push_back(sample);
        }

        if (anchors.size() >= 2)
        {
            std::vector<int64_t> intervals;
            intervals.reserve(anchors.size() - 1);
            for (size_t i = 1; i < anchors.size(); ++i)
                intervals.push_back(anchors[i] - anchors[i - 1]);
            std::sort(intervals.begin(), intervals.end());
            const int64_t medianInterval = intervals[intervals.size() / 2];

            while (!anchors.empty() && anchors.front() > medianInterval / 2)
                anchors.insert(anchors.begin(), juce::jmax<int64_t>(0, anchors.front() - medianInterval));
            while (!anchors.empty() && anchors.back() < sampleData.sourceLengthSamples)
                anchors.push_back(anchors.back() + medianInterval);

            for (size_t beatIndex = 0; beatIndex + 1 < anchors.size(); ++beatIndex)
            {
                const auto beatStart = juce::jlimit<int64_t>(0, sampleData.sourceLengthSamples, anchors[beatIndex]);
                const auto beatEnd = juce::jlimit<int64_t>(0, sampleData.sourceLengthSamples, anchors[beatIndex + 1]);
                if (beatEnd <= beatStart)
                    continue;

                for (int divisionIndex = 0; divisionIndex < safeBeatDivision; ++divisionIndex)
                {
                    const double startAlpha = static_cast<double>(divisionIndex) / static_cast<double>(safeBeatDivision);
                    const double endAlpha = static_cast<double>(divisionIndex + 1) / static_cast<double>(safeBeatDivision);
                    SampleSlice slice;
                    slice.startSample = juce::jlimit<int64_t>(
                        0, sampleData.sourceLengthSamples,
                        beatStart + static_cast<int64_t>(std::llround((beatEnd - beatStart) * startAlpha)));
                    slice.endSample = juce::jlimit<int64_t>(
                        0, sampleData.sourceLengthSamples,
                        beatStart + static_cast<int64_t>(std::llround((beatEnd - beatStart) * endAlpha)));
                    if (slice.endSample <= slice.startSample)
                        slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + 1);
                    slices.push_back(std::move(slice));
                    if (static_cast<int>(slices.size()) >= kMaxInitialSliceCount)
                        break;
                }

                if (static_cast<int>(slices.size()) >= kMaxInitialSliceCount)
                    break;
            }
        }
    }

    if (slices.empty())
    {
        const double beatSeconds = 60.0 / tempoBpm;
        const double sliceSeconds = beatSeconds / static_cast<double>(safeBeatDivision);
        const int64_t sliceSamples = static_cast<int64_t>(std::round(sliceSeconds * sampleData.sourceSampleRate));
        if (sliceSamples <= 0)
            return {};

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
            slices.push_back(std::move(slice));
        }
    }

    for (size_t index = 0; index < slices.size(); ++index)
        fillSliceMetadata(slices[index], static_cast<int>(index), sampleData.sourceLengthSamples);

    return slices;
}

std::vector<SampleSlice> SliceModel::buildTransientSlices(const LoadedSampleData& sampleData,
                                                          const std::vector<int64_t>& transientSamples)
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    if (transientSamples.empty())
        return buildUniformSlices(sampleData, SliceModel::VisibleSliceCount);

    const int64_t minGapSamples = static_cast<int64_t>(juce::jmax(64.0,
        std::round(sampleData.sourceSampleRate * 0.015)));

    std::vector<int64_t> starts;
    starts.reserve(transientSamples.size());
    for (auto samplePosition : transientSamples)
    {
        if (samplePosition < 0 || samplePosition >= sampleData.sourceLengthSamples)
            continue;

        if (!starts.empty() && (samplePosition - starts.back()) < minGapSamples)
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

SampleAnalysisSummary SampleAnalysisEngine::analyzeLoadedSample(const juce::File& file,
                                                               const juce::AudioBuffer<float>& audioBuffer,
                                                               double sourceSampleRate,
                                                               ProgressCallback progressCallback) const
{
    LoadedSampleData sampleData;
    sampleData.audioBuffer.makeCopyOf(audioBuffer, true);
    sampleData.sourceSampleRate = sourceSampleRate > 0.0 ? sourceSampleRate : 44100.0;
    sampleData.numChannels = sampleData.audioBuffer.getNumChannels();
    sampleData.sourceLengthSamples = sampleData.audioBuffer.getNumSamples();
    sampleData.durationSeconds = static_cast<double>(sampleData.sourceLengthSamples)
        / juce::jmax(1.0, sampleData.sourceSampleRate);

    if (file.existsAsFile())
        enrichAnalysisForFile(file, sampleData, std::move(progressCallback));
    else
        sampleData.analysis = buildInternalAnalysis(sampleData);

    return sampleData.analysis;
}

void SampleAnalysisEngine::enrichAnalysisForFile(const juce::File& file,
                                                 LoadedSampleData& sampleData,
                                                 ProgressCallback progressCallback) const
{
    juce::ignoreUnused(file);
    if (progressCallback)
        progressCallback(0.62f, "Analyzing transients...");
    const auto internalSummary = buildInternalAnalysis(sampleData);

    juce::String essentiaFailureReason;
    if (progressCallback)
        progressCallback(0.70f, "Preparing Essentia...");

    if (progressCallback)
        progressCallback(0.76f, "Essentia tempo...");
#if MLRVST_ENABLE_ESSENTIA_NATIVE
    if (const auto essentiaSummary = runEssentiaAnalysis(sampleData, &essentiaFailureReason))
#else
    std::optional<juce::File> essentiaInputFile = createEssentiaAnalysisWaveFile(sampleData, &essentiaFailureReason);
    if (essentiaInputFile)
    {
        if (const auto essentiaSummary = runEssentiaAnalysis(*essentiaInputFile, &essentiaFailureReason))
#endif
    {
        sampleData.analysis = internalSummary;
        sampleData.analysis.essentiaUsed = true;
        sampleData.analysis.beatTickSamples = buildWholeFileDriftCorrectedBeatTicks(
            essentiaSummary->beatTickSamples,
            internalSummary.transientSamples,
            sampleData.sourceLengthSamples);
        if (progressCallback)
            progressCallback(0.9f, "Snapping loop-style transients...");

        if (essentiaSummary->estimatedTempoBpm > 0.0)
            sampleData.analysis.estimatedTempoBpm = essentiaSummary->estimatedTempoBpm;

        if (essentiaSummary->estimatedPitchMidi >= 0)
        {
            sampleData.analysis.estimatedPitchHz = essentiaSummary->estimatedPitchHz;
            sampleData.analysis.estimatedPitchMidi = essentiaSummary->estimatedPitchMidi;
            sampleData.analysis.analysisSource = sampleData.analysis.estimatedTempoBpm > 0.0
                ? essentiaSummary->analysisSource + " + drift-corrected ticks + internal transients"
                : "essentia native pitch + internal transients";
        }
        else
        {
            sampleData.analysis.analysisSource = sampleData.analysis.estimatedTempoBpm > 0.0
                ? essentiaSummary->analysisSource + " + drift-corrected ticks + internal pitch/transients"
                : "ES pitch unresolved";
        }

        if (!(sampleData.analysis.estimatedTempoBpm > 0.0))
            sampleData.analysis.estimatedTempoBpm = internalSummary.estimatedTempoBpm;
        if (sampleData.analysis.estimatedPitchMidi < 0 && internalSummary.estimatedPitchMidi >= 0)
        {
            sampleData.analysis.estimatedPitchHz = internalSummary.estimatedPitchHz;
            sampleData.analysis.estimatedPitchMidi = internalSummary.estimatedPitchMidi;
        }
#if !MLRVST_ENABLE_ESSENTIA_NATIVE
        essentiaInputFile->deleteFile();
#endif
        return;
    }

#if !MLRVST_ENABLE_ESSENTIA_NATIVE
    if (essentiaInputFile)
        essentiaInputFile->deleteFile();
#endif

    if (progressCallback)
        progressCallback(0.86f, "Internal analysis fallback...");
    sampleData.analysis = internalSummary;
    sampleData.analysis.essentiaUsed = false;
    sampleData.analysis.analysisSource = essentiaFailureReason.isNotEmpty()
        ? essentiaFailureReason
        : "internal";
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
            return SliceModel::buildTransientSlices(sampleData,
                                                    state.transientEditSamples.empty()
                                                        ? sampleData.analysis.transientSamples
                                                        : state.transientEditSamples);
        case SampleSliceMode::Beat:
            return SliceModel::buildBeatSlices(sampleData,
                                               state.analyzedTempoBpm > 0.0
                                                   ? state.analyzedTempoBpm
                                                   : sampleData.analysis.estimatedTempoBpm,
                                               state.beatDivision,
                                               sampleData.analysis.beatTickSamples);
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
            LoadCallback callbackToUse,
            ProgressCallback progressCallbackToUse)
        : juce::ThreadPoolJob("SampleModeLoadJob")
        , owner(ownerToUse)
        , file(std::move(fileToLoad))
        , requestId(requestIdToUse)
        , callback(std::move(callbackToUse))
        , progressCallback(std::move(progressCallbackToUse))
    {
    }

    JobStatus runJob() override
    {
        auto progressCallbackCopy = progressCallback;
        auto postProgress = [requestIdValue = requestId, progressCallbackCopy](float progress, const juce::String& statusText)
        {
            if (!progressCallbackCopy)
                return;

            juce::MessageManager::callAsync([progressCallbackCopy, requestIdValue, progress, statusText]() mutable
            {
                progressCallbackCopy(requestIdValue, progress, statusText);
            });
        };

        postProgress(0.12f, "Decoding " + file.getFileName() + "...");
        auto decodeResult = owner.decodeFile(file, requestId, postProgress);

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
    ProgressCallback progressCallback;
};

SampleFileManager::SampleFileManager()
{
    formatManager.registerBasicFormats();
}

SampleFileManager::~SampleFileManager()
{
    cancelPendingLoads();
}

int SampleFileManager::loadFileAsync(const juce::File& file, LoadCallback callback, ProgressCallback progressCallback)
{
    const int requestId = nextRequestId.fetch_add(1);
    threadPool.addJob(new LoadJob(*this,
                                  file,
                                  requestId,
                                  std::move(callback),
                                  std::move(progressCallback)),
                      true);
    return requestId;
}

void SampleFileManager::cancelPendingLoads()
{
    threadPool.removeAllJobs(true, 10000);
}

SampleFileManager::LoadResult SampleFileManager::decodeFile(const juce::File& file,
                                                            int requestId,
                                                            const std::function<void(float progress, const juce::String& statusText)>& progressCallback)
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

    if (progressCallback)
        progressCallback(0.28f, "Decoding audio...");

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
    if (progressCallback)
        progressCallback(0.46f, "Building waveform...");
    buildWaveformPreview(*loadedSample);
    SampleAnalysisEngine().enrichAnalysisForFile(file, *loadedSample, progressCallback);
    if (progressCallback)
        progressCallback(0.98f, "Finalizing analysis...");

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
        resetSampleSpecificStateLocked();
        loadedSample = std::move(sample);
        persistentState.samplePath = sourcePath;
        rebuildSlicesLocked();
        sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
        clearRandomVisibleSliceOverrideLocked();
        isLoading = false;
        analysisProgress = 0.0f;
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
        analysisProgress = 0.05f;
        statusText = "Loading " + file.getFileName() + "...";
        persistentState.samplePath = file.getFullPathName();
        persistentState.analyzedTempoBpm = 0.0;
        persistentState.analyzedPitchHz = 0.0;
        persistentState.analyzedPitchMidi = -1;
        persistentState.essentiaUsed = false;
        persistentState.analysisSource.clear();
        activeRequestId = fileManager.loadFileAsync(file,
            [weakThis](SampleFileManager::LoadResult result) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadResult(std::move(result));
            },
            [weakThis](int requestId, float progress, juce::String loadStatusText) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadProgress(requestId, progress, loadStatusText);
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
        clearRandomVisibleSliceOverrideLocked();
        legacyLoopWindowStartSample = -1;
        isLoading = false;
        analysisProgress = 0.0f;
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
    clearLegacyLoopMonitorState();
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
    snapshot.analysisProgress = analysisProgress;
    snapshot.statusText = statusText;
    int64_t legacyRangeStartSample = 0;
    int64_t legacyRangeEndSample = 0;
    const bool hasLegacyWindow = computeLegacyLoopWindowRangeLocked(legacyRangeStartSample, legacyRangeEndSample);
    snapshot.visibleSliceBankIndex = hasLegacyWindow ? 0 : sliceModel.getVisibleBankIndex();
    snapshot.visibleSliceBankCount = hasLegacyWindow ? 0 : sliceModel.getVisibleBankCount();
    snapshot.canNavigateLeft = hasLegacyWindow ? (legacyRangeStartSample > 0) : sliceModel.canNavigateLeft();
    snapshot.canNavigateRight = hasLegacyWindow
        ? (loadedSample != nullptr && legacyRangeEndSample < loadedSample->sourceLengthSamples)
        : sliceModel.canNavigateRight();
    const bool useLegacyMonitor = persistentState.useLegacyLoopEngine;
    snapshot.isPlaying = useLegacyMonitor
        ? (legacyLoopPlayingAtomic.load(std::memory_order_acquire) != 0)
        : (playingAtomic.load(std::memory_order_acquire) != 0);
    snapshot.activeVisibleSliceSlot = useLegacyMonitor
        ? legacyLoopActiveVisibleSliceSlot.load(std::memory_order_acquire)
        : activeVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.pendingVisibleSliceSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.playbackProgress = useLegacyMonitor
        ? legacyLoopPlaybackProgress.load(std::memory_order_acquire)
        : playbackProgress.load(std::memory_order_acquire);
    snapshot.visibleSlices = getCurrentVisibleSlicesLocked();
    snapshot.sliceMode = persistentState.sliceMode;
    snapshot.triggerMode = persistentState.triggerMode;
    snapshot.useLegacyLoopEngine = persistentState.useLegacyLoopEngine;
    snapshot.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    snapshot.beatDivision = persistentState.beatDivision;
    snapshot.viewZoom = persistentState.viewZoom;
    snapshot.viewScroll = persistentState.viewScroll;
    snapshot.selectedCueIndex = persistentState.selectedCueIndex;
    snapshot.estimatedTempoBpm = persistentState.analyzedTempoBpm;
    snapshot.estimatedPitchHz = persistentState.analyzedPitchHz;
    snapshot.estimatedPitchMidi = persistentState.analyzedPitchMidi;
    snapshot.essentiaUsed = persistentState.essentiaUsed;
    snapshot.analysisSource = persistentState.analysisSource;
    snapshot.cuePoints = persistentState.cuePoints;
    snapshot.transientMarkers = persistentState.transientEditSamples;

    if (loadedSample != nullptr)
    {
        snapshot.samplePath = loadedSample->sourcePath;
        snapshot.displayName = loadedSample->displayName;
        snapshot.sourceSampleRate = loadedSample->sourceSampleRate;
        snapshot.totalSamples = loadedSample->sourceLengthSamples;
        if (snapshot.transientMarkers.empty())
            snapshot.transientMarkers = loadedSample->analysis.transientSamples;
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
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        return rangeStartSample > 0;
    return sliceModel.canNavigateLeft();
}

bool SampleModeEngine::canNavigateVisibleBankRight() const
{
    const juce::ScopedLock lock(stateLock);
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        return loadedSample != nullptr && rangeEndSample < loadedSample->sourceLengthSamples;
    return sliceModel.canNavigateRight();
}

void SampleModeEngine::stepVisibleBank(int delta)
{
    {
        const juce::ScopedLock lock(stateLock);
        clearRandomVisibleSliceOverrideLocked();
        int64_t rangeStartSample = 0;
        int64_t rangeEndSample = 0;
        if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        {
            bool updatedFromBeatTicks = false;
            if (loadedSample != nullptr)
            {
                const float beatsForLoop = legacyLoopBarSelectionToBeats(persistentState.legacyLoopBarSelection);
                const int integerBeatCount = juce::roundToInt(beatsForLoop);
                const auto beatTicks = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples,
                                                               loadedSample->sourceLengthSamples);
                if (integerBeatCount > 0
                    && std::abs(beatsForLoop - static_cast<float>(integerBeatCount)) < 1.0e-4f
                    && beatTicks.size() >= 2)
                {
                    const int currentStartIndex = findNearestBeatTickIndex(beatTicks, rangeStartSample);
                    if (currentStartIndex >= 0)
                    {
                        const int requestedIndex = juce::jlimit(0,
                                                                static_cast<int>(beatTicks.size()) - 1,
                                                                currentStartIndex + (delta * integerBeatCount));
                        int64_t alignedStartSample = 0;
                        int64_t alignedEndSample = 0;
                        if (computeBeatTickAlignedWindowRange(beatTicks,
                                                             loadedSample->sourceLengthSamples,
                                                             beatTicks[static_cast<size_t>(requestedIndex)],
                                                             integerBeatCount,
                                                             alignedStartSample,
                                                             alignedEndSample))
                        {
                            legacyLoopWindowStartSample = alignedStartSample;
                            updatedFromBeatTicks = true;
                        }
                    }
                }
            }

            if (!updatedFromBeatTicks)
            {
                const int64_t windowLength = juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample);
                const int64_t maxWindowStart = juce::jmax<int64_t>(0, loadedSample != nullptr
                                                                      ? (loadedSample->sourceLengthSamples - windowLength)
                                                                      : 0);
                legacyLoopWindowStartSample = juce::jlimit<int64_t>(0,
                                                                    maxWindowStart,
                                                                    rangeStartSample + (static_cast<int64_t>(delta) * windowLength));
            }
            legacyLoopWindowManualAnchor = false;
            fitViewToLegacyLoopWindowLocked();

            const double sr = loadedSample != nullptr ? loadedSample->sourceSampleRate : 0.0;
            const double seconds = sr > 0.0 ? static_cast<double>(legacyLoopWindowStartSample) / sr : 0.0;
            statusText = "Viewing MLR window @ " + juce::String(seconds, 2) + "s";
        }
        else
        {
            sliceModel.stepVisibleBank(delta);
            persistentState.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
            statusText = loadedSample != nullptr
                ? ("Viewing slice bank " + juce::String(sliceModel.getVisibleBankIndex() + 1)
                   + "/" + juce::String(juce::jmax(1, sliceModel.getVisibleBankCount())))
                : juce::String("No sample loaded");
        }
    }

    sendChangeMessage();
}

void SampleModeEngine::randomizeVisibleBank()
{
    bool changed = false;
    {
        const juce::ScopedLock lock(stateLock);
        const auto& allSlices = sliceModel.getAllSlices();
        if (allSlices.empty())
            return;

        std::mt19937 rng(static_cast<uint32_t>(juce::Random::getSystemRandom().nextInt()));
        clearRandomVisibleSliceOverrideLocked();
        randomVisibleSlices = buildDistributedRandomSliceSelection(
            allSlices,
            loadedSample != nullptr ? loadedSample->sourceLengthSamples : 0,
            rng);
        randomVisibleSliceOverrideActive = true;
        statusText = "Viewing distributed random slices";
        changed = true;
    }

    if (changed)
        sendChangeMessage();
}

bool SampleModeEngine::hasVisibleSlice(int visibleSlot) const
{
    SampleSlice slice;
    return resolveVisibleSlice(visibleSlot, slice);
}

bool SampleModeEngine::getVisibleSliceInfo(int visibleSlot, SampleSlice& sliceOut) const
{
    return resolveVisibleSlice(visibleSlot, sliceOut);
}

bool SampleModeEngine::getLegacyLoopSyncInfo(LegacyLoopSyncInfo& syncInfo) const
{
    const juce::ScopedLock lock(stateLock);
    if (loadedSample == nullptr)
        return false;

    syncInfo = {};
    syncInfo.loadedSample = loadedSample;
    syncInfo.visibleSlices = getCurrentVisibleSlicesLocked();
    syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
        ? persistentState.analyzedTempoBpm
        : loadedSample->analysis.estimatedTempoBpm;
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    syncInfo.visibleBankIndex = (randomVisibleSliceOverrideActive || computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        ? -1
        : sliceModel.getVisibleBankIndex();
    syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    syncInfo.sliceMode = persistentState.sliceMode;
    switch (persistentState.sliceMode)
    {
        case SampleSliceMode::Transient:
            syncInfo.markerSamples = persistentState.transientEditSamples.empty()
                ? loadedSample->analysis.transientSamples
                : persistentState.transientEditSamples;
            break;
        case SampleSliceMode::Manual:
            syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
            {
                if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                    syncInfo.markerSamples.push_back(cue.samplePosition);
            }
            break;
        case SampleSliceMode::Beat:
        case SampleSliceMode::Uniform:
        default:
            syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
            for (const auto& slice : syncInfo.visibleSlices)
            {
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
            break;
    }
    syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                            syncInfo.visibleSlices,
                                                            syncInfo.analyzedTempoBpm,
                                                            persistentState.legacyLoopBarSelection);
    return true;
}

bool SampleModeEngine::resolveLegacyLoopTriggerSyncInfo(int preferredVisibleSlot,
                                                        int sliceId,
                                                        int64_t sliceStartSample,
                                                        LegacyLoopSyncInfo& syncInfo) const
{
    const juce::ScopedLock lock(stateLock);
    if (loadedSample == nullptr)
        return false;

    int targetSliceIndex = -1;
    const int clampedSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, preferredVisibleSlot);
    const auto& allSlices = sliceModel.getAllSlices();
    int64_t legacyRangeStartSample = 0;
    int64_t legacyRangeEndSample = 0;
    const bool useLegacyWindowSlices = computeLegacyLoopWindowRangeLocked(legacyRangeStartSample, legacyRangeEndSample)
        && sliceId < 0
        && sliceStartSample < 0;
    const bool useRandomVisibleSlices = randomVisibleSliceOverrideActive
        && !useLegacyWindowSlices
        && sliceId < 0
        && sliceStartSample < 0;

    if (useLegacyWindowSlices)
    {
        syncInfo = {};
        syncInfo.loadedSample = loadedSample;
        syncInfo.visibleSlices = buildLegacyLoopWindowVisibleSlicesLocked(legacyRangeStartSample, legacyRangeEndSample);
        syncInfo.visibleBankIndex = -1;
        syncInfo.triggerVisibleSlot = clampedSlot;
        syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : loadedSample->analysis.estimatedTempoBpm;
        syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
        syncInfo.sliceMode = persistentState.sliceMode;
        if (persistentState.sliceMode == SampleSliceMode::Transient)
        {
            syncInfo.markerSamples = persistentState.transientEditSamples.empty()
                ? loadedSample->analysis.transientSamples
                : persistentState.transientEditSamples;
        }
        else if (persistentState.sliceMode == SampleSliceMode::Manual)
        {
            syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
            {
                if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                    syncInfo.markerSamples.push_back(cue.samplePosition);
            }
        }
        else
        {
            syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
            for (const auto& slice : syncInfo.visibleSlices)
            {
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
        }
        syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                                syncInfo.visibleSlices,
                                                                syncInfo.analyzedTempoBpm,
                                                                persistentState.legacyLoopBarSelection);
        return true;
    }

    if (useRandomVisibleSlices)
    {
        syncInfo = {};
        syncInfo.loadedSample = loadedSample;
        syncInfo.visibleSlices = randomVisibleSlices;
        syncInfo.visibleBankIndex = -1;
        syncInfo.triggerVisibleSlot = clampedSlot;
        syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : loadedSample->analysis.estimatedTempoBpm;
        syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
        syncInfo.sliceMode = persistentState.sliceMode;
        syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
        for (const auto& slice : syncInfo.visibleSlices)
        {
            if (slice.id >= 0 && slice.endSample > slice.startSample)
                syncInfo.markerSamples.push_back(slice.startSample);
        }
        syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                                syncInfo.visibleSlices,
                                                                syncInfo.analyzedTempoBpm,
                                                                persistentState.legacyLoopBarSelection);
        return true;
    }

    if (sliceId >= 0 || sliceStartSample >= 0)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id != sliceId)
                continue;
            if (sliceStartSample < 0 || slice.startSample == sliceStartSample)
            {
                targetSliceIndex = i;
                break;
            }
        }

        if (targetSliceIndex < 0 && sliceStartSample >= 0)
        {
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
            {
                const auto& slice = allSlices[static_cast<size_t>(i)];
                const int64_t distance = std::abs(slice.startSample - sliceStartSample);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    targetSliceIndex = i;
                    if (distance == 0)
                        break;
                }
            }
        }
    }

    const int pendingSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    if (targetSliceIndex < 0 && pendingTriggerSliceValid && pendingSlot == clampedSlot)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id == pendingTriggerSlice.id
                && slice.startSample == pendingTriggerSlice.startSample
                && slice.endSample == pendingTriggerSlice.endSample)
            {
                targetSliceIndex = i;
                break;
            }
        }
    }

    if (targetSliceIndex < 0)
    {
        const auto visibleSlices = getCurrentVisibleSlicesLocked();
        const auto& currentSlice = visibleSlices[static_cast<size_t>(clampedSlot)];
        if (currentSlice.id >= 0)
        {
            for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
            {
                const auto& slice = allSlices[static_cast<size_t>(i)];
                if (slice.id == currentSlice.id
                    && slice.startSample == currentSlice.startSample
                    && slice.endSample == currentSlice.endSample)
                {
                    targetSliceIndex = i;
                    break;
                }
            }
        }
    }

    if (targetSliceIndex < 0 || targetSliceIndex >= static_cast<int>(allSlices.size()))
        return false;

    return buildLegacyLoopSyncInfoForSliceIndexLocked(targetSliceIndex, syncInfo);
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
                                                 bool enabled,
                                                 TimeStretchBackend backend)
{
#if !(MLRVST_ENABLE_SOUNDTOUCH || MLRVST_ENABLE_BUNGEE)
    juce::ignoreUnused(playbackRate, pitchSemitones, enabled, backend);
    return;
#else
    const auto sanitizedBackend = sanitizeTimeStretchBackend(static_cast<int>(backend));
    if (sanitizedBackend == TimeStretchBackend::Resample
        || !isTimeStretchBackendAvailable(sanitizedBackend))
    {
        const juce::ScopedLock lock(stateLock);
        keyLockCacheEnabled = false;
        keyLockCaches.clear();
        keyLockCacheBuildInFlight = false;
        ++keyLockCacheGeneration;
        return;
    }

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
        keyLockCacheBackend = sanitizedBackend;

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

        const int currentBankIndex = randomVisibleSliceOverrideActive ? -1 : sliceModel.getVisibleBankIndex();
        const bool matchesExisting = !keyLockCaches.empty()
            && currentBankIndex == keyLockCacheVisibleBankIndex
            && std::abs(keyLockCachePlaybackRate - quantizedRate) <= kKeyLockCacheRateTolerance
            && std::abs(keyLockCachePitchSemitones - quantizedPitch) <= kKeyLockCachePitchTolerance
            && keyLockCacheBackend == sanitizedBackend;
        if (matchesExisting || keyLockCacheBuildInFlight)
            return;

        sampleData = loadedSample;
        visibleSlices = getCurrentVisibleSlicesLocked();
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
                        TimeStretchBackend backendToUse,
                        int visibleBankIndexToUse)
            : juce::ThreadPoolJob("FlipKeyLockCache")
            , owner(ownerToUse)
            , sampleData(std::move(sampleDataToUse))
            , visibleSlices(std::move(visibleSlicesToUse))
            , generation(generationToUse)
            , playbackRate(playbackRateToUse)
            , pitchSemitones(pitchSemitonesToUse)
            , backend(backendToUse)
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

                    if (auto cache = buildStretchedSliceBuffer(*sampleData,
                                                               slice,
                                                               playbackRate,
                                                               pitchSemitones,
                                                               backend))
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
        TimeStretchBackend backend = TimeStretchBackend::SoundTouch;
        int visibleBankIndex = 0;
    };

    keyLockCacheThreadPool.addJob(
        new KeyLockCacheJob(weakThis,
                            std::move(sampleData),
                            visibleSlices,
                            generation,
                            quantizedRate,
                            quantizedPitch,
                            sanitizedBackend,
                            visibleBankIndex),
        true);
#endif
}

bool SampleModeEngine::triggerResolvedSlice(const SampleSlice& slice,
                                            int resolvedVisibleSlot,
                                            SampleTriggerMode triggerMode)
{
    const int fadeSamples = juce::jmax(16,
        juce::jmin(kDefaultSliceFadeSamples,
                   static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    for (auto& voice : playbackVoices)
    {
        if (voice.isActive())
            voice.beginRelease(fadeSamples);
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

    targetVoice->trigger(slice, resolvedVisibleSlot, nextVoiceId++, triggerMode == SampleTriggerMode::Loop, fadeSamples);
    activeVisibleSliceSlot.store(resolvedVisibleSlot, std::memory_order_release);
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(slice.normalizedStart, std::memory_order_release);
    playingAtomic.store(1, std::memory_order_release);
    return true;
}

bool SampleModeEngine::resolveRecordedSlice(int sliceId,
                                            int64_t sliceStartSample,
                                            SampleSlice& sliceOut,
                                            int& resolvedVisibleSlot) const
{
    const juce::ScopedLock lock(stateLock);
    const auto& allSlices = sliceModel.getAllSlices();
    if (allSlices.empty())
        return false;

    int foundIndex = -1;

    if (sliceId >= 0)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& candidate = allSlices[static_cast<size_t>(i)];
            if (candidate.id != sliceId)
                continue;

            if (sliceStartSample < 0 || candidate.startSample == sliceStartSample)
            {
                foundIndex = i;
                break;
            }
        }
    }

    if (foundIndex < 0 && sliceStartSample >= 0)
    {
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& candidate = allSlices[static_cast<size_t>(i)];
            const int64_t distance = std::abs(candidate.startSample - sliceStartSample);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                foundIndex = i;
                if (distance == 0)
                    break;
            }
        }
    }

    if (foundIndex < 0)
        return false;

    sliceOut = allSlices[static_cast<size_t>(foundIndex)];
    if (sliceOut.id < 0 || sliceOut.endSample <= sliceOut.startSample)
        return false;

    const int visibleBankIndex = sliceModel.getVisibleBankIndex();
    const int visibleStart = visibleBankIndex * SliceModel::VisibleSliceCount;
    if (foundIndex >= visibleStart && foundIndex < (visibleStart + SliceModel::VisibleSliceCount))
        resolvedVisibleSlot = foundIndex - visibleStart;
    else
        resolvedVisibleSlot = -1;

    if (resolvedVisibleSlot < 0 && randomVisibleSliceOverrideActive)
    {
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const auto& visibleSlice = randomVisibleSlices[static_cast<size_t>(slot)];
            if (visibleSlice.id == sliceOut.id
                && visibleSlice.startSample == sliceOut.startSample
                && visibleSlice.endSample == sliceOut.endSample)
            {
                resolvedVisibleSlot = slot;
                break;
            }
        }
    }

    return true;
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

            const auto visibleSlices = getCurrentVisibleSlicesLocked();
            const auto& resolvedSlice = visibleSlices[static_cast<size_t>(visibleSlot)];
            if (resolvedSlice.id < 0 || resolvedSlice.endSample <= resolvedSlice.startSample)
                return false;

            slice = resolvedSlice;
        }
    }

    return triggerResolvedSlice(slice, visibleSlot, triggerMode);
}

bool SampleModeEngine::triggerRecordedSlice(int preferredVisibleSlot,
                                            int sliceId,
                                            int64_t sliceStartSample,
                                            bool forceLoop)
{
    SampleSlice slice;
    int resolvedVisibleSlot = preferredVisibleSlot;
    if (!resolveRecordedSlice(sliceId, sliceStartSample, slice, resolvedVisibleSlot))
        return triggerVisibleSlice(preferredVisibleSlot, forceLoop);

    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    {
        const juce::ScopedLock lock(stateLock);
        triggerMode = forceLoop ? SampleTriggerMode::Loop : persistentState.triggerMode;
    }

    return triggerResolvedSlice(slice, resolvedVisibleSlot, triggerMode);
}

void SampleModeEngine::updateLegacyLoopMonitorState(bool isPlaying,
                                                    int visibleSlot,
                                                    float playbackProgressNormalized)
{
    legacyLoopPlayingAtomic.store(isPlaying ? 1 : 0, std::memory_order_release);
    legacyLoopActiveVisibleSliceSlot.store(juce::jlimit(-1, SliceModel::VisibleSliceCount - 1, visibleSlot),
                                           std::memory_order_release);
    legacyLoopPlaybackProgress.store(juce::jlimit(-1.0f, 1.0f, playbackProgressNormalized),
                                     std::memory_order_release);
}

void SampleModeEngine::clearLegacyLoopMonitorState()
{
    legacyLoopPlayingAtomic.store(0, std::memory_order_release);
    legacyLoopActiveVisibleSliceSlot.store(-1, std::memory_order_release);
    legacyLoopPlaybackProgress.store(-1.0f, std::memory_order_release);
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
    TimeStretchBackend activeKeyLockBackend = TimeStretchBackend::Resample;

    bool renderedAnything = false;
    int newestActiveSlot = -1;
    uint64_t newestVoiceId = 0;
    float latestProgress = playbackProgress.load(std::memory_order_acquire);
    bool canUseKeyLockForAllVoices = false;

    if (preferHighQualityKeyLock)
    {
        const juce::ScopedLock stateScopedLock(stateLock);
        availableKeyLockCaches = keyLockCaches;
        activeKeyLockBackend = keyLockCacheBackend;
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
        const bool deferHotSwapForBackend = activeKeyLockBackend == TimeStretchBackend::Bungee;
        canUseKeyLockForAllVoices = true;
        for (auto& voice : playbackVoices)
        {
            if (!voice.isActive())
                continue;

            const auto existing = voice.getStretchedBuffer();
            const bool hasMatchingExisting = existing != nullptr
                && matchesKeyLockRequest(*existing, voice.getActiveSlice(), playbackRate, pitchSemitones);
            if (hasMatchingExisting)
                continue;

            if (deferHotSwapForBackend && existing != nullptr)
                continue;

            if (findAvailableCache(voice.getActiveSlice()) == nullptr)
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
                    if (deferHotSwapForBackend && existing != nullptr)
                        continue;

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
        if (!(state.analyzedTempoBpm > 0.0))
            state.analyzedTempoBpm = loadedSample->analysis.estimatedTempoBpm;
        if (!(state.analyzedPitchHz > 0.0))
            state.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
        if (state.analyzedPitchMidi < 0)
            state.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
        state.essentiaUsed = loadedSample->analysis.essentiaUsed;
        if (state.analysisSource.isEmpty())
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
        clearRandomVisibleSliceOverrideLocked();
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
        clearRandomVisibleSliceOverrideLocked();
        if (mode != SampleSliceMode::Manual)
            persistentState.selectedCueIndex = -1;
        if (mode == SampleSliceMode::Transient)
            materializeTransientMarkersLocked();
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

void SampleModeEngine::setLegacyLoopEngineEnabled(bool enabled)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.useLegacyLoopEngine = enabled;
        if (enabled)
            clearRandomVisibleSliceOverrideLocked();
        if (enabled && legacyLoopWindowStartSample < 0)
        {
            legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
            legacyLoopWindowManualAnchor = false;
        }
        else if (!enabled)
        {
            legacyLoopWindowStartSample = -1;
            legacyLoopWindowManualAnchor = false;
        }
        if (persistentState.legacyLoopBarSelection > 0)
            fitViewToLegacyLoopWindowLocked();
    }
    if (!enabled)
        clearLegacyLoopMonitorState();
    sendChangeMessage();
}

bool SampleModeEngine::isLegacyLoopEngineEnabled() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.useLegacyLoopEngine;
}

void SampleModeEngine::setLegacyLoopBarSelection(int selection)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (selection > 0)
            clearRandomVisibleSliceOverrideLocked();
        switch (selection)
        {
            case 25:
            case 50:
            case 100:
            case 200:
            case 400:
            case 800:
                persistentState.legacyLoopBarSelection = selection;
                break;
            default:
                persistentState.legacyLoopBarSelection = 0;
                break;
        }
        if (persistentState.legacyLoopBarSelection > 0
            && persistentState.useLegacyLoopEngine
            && legacyLoopWindowStartSample < 0)
        {
            legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
            legacyLoopWindowManualAnchor = false;
        }
        else if (persistentState.legacyLoopBarSelection == 0)
        {
            legacyLoopWindowStartSample = -1;
            legacyLoopWindowManualAnchor = false;
        }
        if (persistentState.legacyLoopBarSelection > 0)
        {
            if (legacyLoopWindowStartSample < 0)
            {
                legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
                legacyLoopWindowManualAnchor = false;
            }
            fitViewToLegacyLoopWindowLocked();
        }
    }
    sendChangeMessage();
}

int SampleModeEngine::getLegacyLoopBarSelection() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.legacyLoopBarSelection;
}

bool SampleModeEngine::nudgeLegacyLoopWindowByAnchorDelta(int delta)
{
    if (delta == 0)
        return false;

    bool changed = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || !persistentState.useLegacyLoopEngine
            || persistentState.legacyLoopBarSelection <= 0)
        {
            return false;
        }

        const auto candidates = buildLegacyLoopAnchorCandidatesLocked();
        if (candidates.empty())
            return false;

        int64_t currentRangeStartSample = 0;
        int64_t currentRangeEndSample = 0;
        const bool hasCurrentRange = computeLegacyLoopWindowRangeLocked(currentRangeStartSample, currentRangeEndSample);
        const int64_t currentAnchor = hasCurrentRange
            ? currentRangeStartSample
            : (legacyLoopWindowStartSample >= 0 ? legacyLoopWindowStartSample : candidates.front());

        int currentIndex = findNearestBeatTickIndex(candidates, currentAnchor);
        if (currentIndex < 0)
            currentIndex = 0;

        const int targetIndex = juce::jlimit(0,
                                             static_cast<int>(candidates.size()) - 1,
                                             currentIndex + delta);
        const int64_t targetAnchor = candidates[static_cast<size_t>(targetIndex)];
        if (targetAnchor != legacyLoopWindowStartSample || !legacyLoopWindowManualAnchor)
        {
            legacyLoopWindowStartSample = targetAnchor;
            legacyLoopWindowManualAnchor = true;
            const double seconds = loadedSample->sourceSampleRate > 0.0
                ? static_cast<double>(legacyLoopWindowStartSample) / loadedSample->sourceSampleRate
                : 0.0;
            statusText = "Viewing MLR selection @ " + juce::String(seconds, 2) + "s";
            changed = true;
        }
    }

    if (changed)
        sendChangeMessage();
    return changed;
}

void SampleModeEngine::scaleAnalyzedTempo(double factor)
{
    if (!std::isfinite(factor) || factor <= 0.0)
        return;

    {
        const juce::ScopedLock lock(stateLock);
        const double currentTempo = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : (loadedSample != nullptr ? loadedSample->analysis.estimatedTempoBpm : 0.0);
        if (!(currentTempo > 0.0))
            return;

        persistentState.analyzedTempoBpm = juce::jlimit(20.0, 400.0, currentTempo * factor);
        if (persistentState.analysisSource.isEmpty())
            persistentState.analysisSource = "manual tempo";
    }

    sendChangeMessage();
}

double SampleModeEngine::getAnalyzedTempoBpm() const
{
    const juce::ScopedLock lock(stateLock);
    if (persistentState.analyzedTempoBpm > 0.0)
        return persistentState.analyzedTempoBpm;
    return loadedSample != nullptr ? loadedSample->analysis.estimatedTempoBpm : 0.0;
}

void SampleModeEngine::setBeatDivision(int division)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.beatDivision = juce::jlimit(1, 8, division);
        persistentState.storedSlices.clear();
        clearRandomVisibleSliceOverrideLocked();
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

int SampleModeEngine::moveCuePoint(int cueIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || cueIndex < 0
            || cueIndex >= static_cast<int>(persistentState.cuePoints.size()))
        {
            return -1;
        }

        const int cueId = persistentState.cuePoints[static_cast<size_t>(cueIndex)].id;
        const int64_t cueSample = juce::jlimit<int64_t>(
            1,
            juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1),
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        persistentState.cuePoints[static_cast<size_t>(cueIndex)].samplePosition = cueSample;
        std::sort(persistentState.cuePoints.begin(), persistentState.cuePoints.end(),
                  [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].id == cueId)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                newIndex = static_cast<int>(i);
                break;
            }
        }
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return newIndex;
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

void SampleModeEngine::materializeTransientMarkersLocked()
{
    if (loadedSample == nullptr)
        return;

    if (!persistentState.transientEditSamples.empty())
        return;

    persistentState.transientEditSamples = loadedSample->analysis.transientSamples;
    persistentState.transientEditSamples.erase(
        std::remove_if(persistentState.transientEditSamples.begin(),
                       persistentState.transientEditSamples.end(),
                       [this](int64_t sample)
                       {
                           return sample <= 0 || sample >= loadedSample->sourceLengthSamples;
                       }),
        persistentState.transientEditSamples.end());
    std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
    persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                           persistentState.transientEditSamples.end()),
                                               persistentState.transientEditSamples.end());
}

void SampleModeEngine::resetSampleSpecificStateLocked()
{
    persistentState.samplePath.clear();
    persistentState.visibleSliceBankIndex = 0;
    persistentState.viewScroll = 0.0f;
    persistentState.selectedCueIndex = -1;
    persistentState.analyzedTempoBpm = 0.0;
    persistentState.analyzedPitchHz = 0.0;
    persistentState.analyzedPitchMidi = -1;
    persistentState.analysisSource.clear();
    persistentState.cuePoints.clear();
    persistentState.transientEditSamples.clear();
    persistentState.storedSlices.clear();
    clearRandomVisibleSliceOverrideLocked();
    legacyLoopWindowStartSample = -1;
    legacyLoopWindowManualAnchor = false;
}

int SampleModeEngine::createTransientMarkerAtNormalizedPosition(float normalizedPosition)
{
    int markerIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        materializeTransientMarkersLocked();
        const int64_t markerSample = static_cast<int64_t>(
            std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                         * static_cast<float>(loadedSample->sourceLengthSamples - 1)));
        if (markerSample <= 0 || markerSample >= loadedSample->sourceLengthSamples)
            return -1;

        persistentState.transientEditSamples.push_back(markerSample);
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());
        auto it = std::find(persistentState.transientEditSamples.begin(),
                            persistentState.transientEditSamples.end(),
                            markerSample);
        if (it != persistentState.transientEditSamples.end())
            markerIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return markerIndex;
}

int SampleModeEngine::moveTransientMarker(int markerIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
        {
            return -1;
        }

        const int64_t markerSample = juce::jlimit<int64_t>(
            1,
            juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1),
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] = markerSample;
        persistentState.transientEditSamples.erase(
            std::remove_if(persistentState.transientEditSamples.begin(),
                           persistentState.transientEditSamples.end(),
                           [this](int64_t sample)
                           {
                               return sample <= 0 || sample >= loadedSample->sourceLengthSamples;
                           }),
            persistentState.transientEditSamples.end());
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());
        auto it = std::lower_bound(persistentState.transientEditSamples.begin(),
                                   persistentState.transientEditSamples.end(),
                                   markerSample);
        if (it != persistentState.transientEditSamples.end())
            newIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        if (newIndex < 0 && !persistentState.transientEditSamples.empty())
            newIndex = juce::jlimit(0, static_cast<int>(persistentState.transientEditSamples.size()) - 1, markerIndex);
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::moveTransientMarkerToNearestDetectedTransient(int markerIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
        {
            return -1;
        }

        const int64_t totalSamples = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples);
        const int64_t targetSample = juce::jlimit<int64_t>(
            1,
            totalSamples - 1,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(totalSamples - 1))));

        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex - 1)] + 1)
            : 1;
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.transientEditSamples.size()))
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex + 1)] - 1)
            : (totalSamples - 1);
        if (upperBound <= lowerBound)
            return -1;

        int64_t snappedSample = juce::jlimit(lowerBound, upperBound, targetSample);
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        for (const auto detectedSample : loadedSample->analysis.transientSamples)
        {
            if (detectedSample < lowerBound || detectedSample > upperBound)
                continue;

            const int64_t distance = std::abs(detectedSample - targetSample);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                snappedSample = detectedSample;
            }
        }

        if (persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] == snappedSample)
            return markerIndex;

        persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] = snappedSample;
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());

        auto it = std::lower_bound(persistentState.transientEditSamples.begin(),
                                   persistentState.transientEditSamples.end(),
                                   snappedSample);
        if (it != persistentState.transientEditSamples.end())
            newIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        if (newIndex < 0 && !persistentState.transientEditSamples.empty())
            newIndex = juce::jlimit(0, static_cast<int>(persistentState.transientEditSamples.size()) - 1, markerIndex);

        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::findTransientMarkerIndexForVisibleSlotLocked(int visibleSlot)
{
    materializeTransientMarkersLocked();
    if (loadedSample == nullptr
        || visibleSlot < 0
        || visibleSlot >= SliceModel::VisibleSliceCount
        || persistentState.transientEditSamples.empty())
    {
        return -1;
    }

    const auto visibleSlices = getCurrentVisibleSlicesLocked();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    int markerIndex = -1;
    int64_t bestMarkerDistance = std::numeric_limits<int64_t>::max();
    for (int index = 0; index < static_cast<int>(persistentState.transientEditSamples.size()); ++index)
    {
        const int64_t markerSample = persistentState.transientEditSamples[static_cast<size_t>(index)];
        const int64_t distance = std::abs(markerSample - slice.startSample);
        if (distance < bestMarkerDistance)
        {
            bestMarkerDistance = distance;
            markerIndex = index;
            if (distance == 0)
                break;
        }
    }

    return markerIndex;
}

bool SampleModeEngine::moveVisibleSliceToPosition(int visibleSlot, float normalizedPosition)
{
    int markerIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || persistentState.sliceMode != SampleSliceMode::Transient)
            return false;

        markerIndex = findTransientMarkerIndexForVisibleSlotLocked(visibleSlot);
    }

    if (markerIndex < 0)
        return false;

    return moveTransientMarker(markerIndex, normalizedPosition) >= 0;
}

bool SampleModeEngine::moveVisibleSliceToNearestTransient(int visibleSlot, float normalizedPosition)
{
    int markerIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || persistentState.sliceMode != SampleSliceMode::Transient)
            return false;

        markerIndex = findTransientMarkerIndexForVisibleSlotLocked(visibleSlot);
    }

    if (markerIndex < 0)
        return false;

    return moveTransientMarkerToNearestDetectedTransient(markerIndex, normalizedPosition) >= 0;
}

bool SampleModeEngine::deleteTransientMarker(int markerIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (markerIndex < 0 || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
            return false;

        persistentState.transientEditSamples.erase(persistentState.transientEditSamples.begin() + markerIndex);
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
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
    const auto visibleSlices = getCurrentVisibleSlicesLocked();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return false;

    sliceOut = slice;
    return true;
}

int64_t SampleModeEngine::getDefaultLegacyLoopWindowStartSampleLocked() const
{
    const auto visibleSlices = sliceModel.getVisibleSlices();
    int64_t rangeStartSample = std::numeric_limits<int64_t>::max();
    for (const auto& slice : visibleSlices)
    {
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;
        rangeStartSample = juce::jmin(rangeStartSample, slice.startSample);
    }

    if (rangeStartSample == std::numeric_limits<int64_t>::max())
        return 0;
    return rangeStartSample;
}

bool SampleModeEngine::computeLegacyLoopWindowRangeLocked(int64_t& rangeStartSample, int64_t& rangeEndSample) const
{
    rangeStartSample = 0;
    rangeEndSample = 0;

    if (loadedSample == nullptr
        || persistentState.legacyLoopBarSelection <= 0
        || loadedSample->sourceSampleRate <= 0.0
        || !(persistentState.analyzedTempoBpm > 0.0))
    {
        return false;
    }

    const float beatsForLoop = legacyLoopBarSelectionToBeats(persistentState.legacyLoopBarSelection);
    if (!(beatsForLoop > 0.0f))
        return false;

    const int64_t sourceLength = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples);
    const int64_t defaultStart = getDefaultLegacyLoopWindowStartSampleLocked();
    const int64_t requestedStart = legacyLoopWindowStartSample >= 0 ? legacyLoopWindowStartSample : defaultStart;

    const int integerBeatCount = juce::roundToInt(beatsForLoop);
    const auto beatTicks = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples, sourceLength);
    const bool usesWholeBeatCount = integerBeatCount > 0
        && std::abs(beatsForLoop - static_cast<float>(integerBeatCount)) < 1.0e-4f;
    if (usesWholeBeatCount && beatTicks.size() >= 2)
    {
        if (legacyLoopWindowManualAnchor)
        {
            rangeStartSample = juce::jlimit<int64_t>(0, sourceLength - 1, requestedStart);
            const double startBeat = computeBeatTickPosition(beatTicks, rangeStartSample);
            rangeEndSample = samplePositionFromBeatTickPosition(beatTicks,
                                                                startBeat + static_cast<double>(integerBeatCount),
                                                                sourceLength);
            rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1, sourceLength, rangeEndSample);
            return rangeEndSample > rangeStartSample;
        }

        if (computeBeatTickAlignedWindowRange(beatTicks,
                                             sourceLength,
                                             requestedStart,
                                             integerBeatCount,
                                             rangeStartSample,
                                             rangeEndSample))
        {
            return true;
        }
    }

    const double samplesPerBeat = (60.0 / persistentState.analyzedTempoBpm) * loadedSample->sourceSampleRate;
    const int64_t targetLength = static_cast<int64_t>(std::llround(static_cast<double>(beatsForLoop) * samplesPerBeat));
    if (targetLength <= 0)
        return false;

    const int64_t maxWindowStart = juce::jmax<int64_t>(0, sourceLength - targetLength);
    rangeStartSample = legacyLoopWindowManualAnchor
        ? juce::jlimit<int64_t>(0, sourceLength - 1, requestedStart)
        : juce::jlimit<int64_t>(0, maxWindowStart, requestedStart);
    rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1, sourceLength, rangeStartSample + targetLength);
    return rangeEndSample > rangeStartSample;
}

void SampleModeEngine::fitViewToLegacyLoopWindowLocked()
{
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (loadedSample == nullptr
        || loadedSample->sourceLengthSamples <= 1
        || !computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
    {
        return;
    }

    const float rangeStartNorm = safeNormalizedPosition(rangeStartSample, loadedSample->sourceLengthSamples);
    const float rangeEndNorm = safeNormalizedPosition(rangeEndSample, loadedSample->sourceLengthSamples);
    const float visibleLength = juce::jlimit(1.0f / kMaxViewZoom, 1.0f, juce::jmax(1.0e-4f, rangeEndNorm - rangeStartNorm));

    persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, 1.0f / visibleLength);
    persistentState.viewScroll = juce::jlimit(0.0f,
                                              juce::jmax(0.0f, 1.0f - visibleLength),
                                              rangeStartNorm);
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SampleModeEngine::buildLegacyLoopWindowVisibleSlicesLocked(int64_t rangeStartSample,
                                                                                                                    int64_t rangeEndSample) const
{
    std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    if (loadedSample == nullptr || rangeEndSample <= rangeStartSample)
        return visibleSlices;

    if (persistentState.sliceMode == SampleSliceMode::Transient)
    {
        const int windowLength = static_cast<int>(juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample));
        juce::AudioBuffer<float> localWindow(1, windowLength);
        localWindow.clear();

        const int sourceChannels = juce::jmax(1, loadedSample->audioBuffer.getNumChannels());
        for (int sampleIndex = 0; sampleIndex < windowLength; ++sampleIndex)
        {
            const int sourceIndex = static_cast<int>(rangeStartSample) + sampleIndex;
            float mono = 0.0f;
            for (int ch = 0; ch < sourceChannels; ++ch)
                mono += loadedSample->audioBuffer.getSample(ch, sourceIndex);
            localWindow.setSample(0, sampleIndex, mono / static_cast<float>(sourceChannels));
        }

        auto localTransientSamples = detectLoopStyleTransientSamples(localWindow,
                                                                     loadedSample->sourceSampleRate);

        const auto& sourceTransientMarkers = persistentState.transientEditSamples.empty()
            ? loadedSample->analysis.transientSamples
            : persistentState.transientEditSamples;
        for (const auto marker : sourceTransientMarkers)
        {
            if (marker < rangeStartSample || marker >= rangeEndSample)
                continue;
            localTransientSamples.push_back(marker - rangeStartSample);
        }

        std::sort(localTransientSamples.begin(), localTransientSamples.end());
        localTransientSamples.erase(std::unique(localTransientSamples.begin(), localTransientSamples.end()),
                                    localTransientSamples.end());

        const auto localStarts = buildLoopStyleAnchorStarts(windowLength, localTransientSamples);

        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            SampleSlice slice;
            slice.id = slot;
            slice.startSample = rangeStartSample + localStarts[static_cast<size_t>(slot)];
            slice.endSample = (slot + 1 < SliceModel::VisibleSliceCount)
                ? juce::jmax(slice.startSample + 1,
                             rangeStartSample + localStarts[static_cast<size_t>(slot + 1)])
                : rangeEndSample;
            slice.transientDerived = true;
            fillSliceMetadata(slice, slot, loadedSample->sourceLengthSamples);
            visibleSlices[static_cast<size_t>(slot)] = slice;
        }

        return visibleSlices;
    }

    std::vector<int64_t> candidates;
    if (persistentState.sliceMode == SampleSliceMode::Manual)
    {
        candidates.reserve(persistentState.cuePoints.size() + 1u);
        for (const auto& cue : persistentState.cuePoints)
            candidates.push_back(cue.samplePosition);
    }
    else
    {
        const auto& allSlices = sliceModel.getAllSlices();
        candidates.reserve(allSlices.size());
        for (const auto& slice : allSlices)
            candidates.push_back(slice.startSample);
    }

    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [rangeStartSample, rangeEndSample](int64_t sample)
                                    {
                                        return sample < rangeStartSample || sample >= rangeEndSample;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    const int64_t rangeLength = juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample);
    const int64_t nominalStep = juce::jmax<int64_t>(1,
        static_cast<int64_t>(std::llround(static_cast<double>(rangeLength - 1)
                                          / static_cast<double>(juce::jmax(1, SliceModel::VisibleSliceCount - 1)))));
    const int64_t maxSnapDistance = juce::jmax<int64_t>(1, (nominalStep * 45) / 100);
    std::array<int64_t, SliceModel::VisibleSliceCount> starts {};
    int64_t previousStart = rangeStartSample - 1;
    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        int64_t chosen = rangeStartSample;
        int64_t target = rangeStartSample;
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        if (slot == 0)
        {
            chosen = rangeStartSample;
        }
        else
        {
            target = rangeStartSample
                + static_cast<int64_t>((static_cast<double>(slot) / static_cast<double>(SliceModel::VisibleSliceCount - 1))
                                       * static_cast<double>(rangeLength - 1));
            chosen = target;
            auto it = std::lower_bound(candidates.begin(), candidates.end(), target);
            if (it != candidates.end())
            {
                bestDistance = std::abs(*it - target);
                chosen = *it;
            }
            if (it != candidates.begin())
            {
                const int64_t prev = *std::prev(it);
                const int64_t distance = std::abs(prev - target);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    chosen = prev;
                }
            }
        }

        if (slot > 0 && bestDistance > maxSnapDistance)
            chosen = target;

        chosen = juce::jlimit<int64_t>(rangeStartSample, rangeEndSample - 1, juce::jmax(previousStart + 1, chosen));
        starts[static_cast<size_t>(slot)] = chosen;
        previousStart = chosen;
    }

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        SampleSlice slice;
        slice.id = slot;
        slice.startSample = starts[static_cast<size_t>(slot)];
        slice.endSample = (slot + 1 < SliceModel::VisibleSliceCount)
            ? juce::jmax(starts[static_cast<size_t>(slot)] + 1, starts[static_cast<size_t>(slot + 1)])
            : rangeEndSample;
        slice.transientDerived = (persistentState.sliceMode == SampleSliceMode::Transient);
        slice.manualDerived = (persistentState.sliceMode == SampleSliceMode::Manual);
        fillSliceMetadata(slice, slot, loadedSample->sourceLengthSamples);
        visibleSlices[static_cast<size_t>(slot)] = slice;
    }

    return visibleSlices;
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SampleModeEngine::getCurrentVisibleSlicesLocked() const
{
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        return buildLegacyLoopWindowVisibleSlicesLocked(rangeStartSample, rangeEndSample);

    if (randomVisibleSliceOverrideActive)
        return randomVisibleSlices;

    return sliceModel.getVisibleSlices();
}

std::vector<int64_t> SampleModeEngine::buildLegacyLoopAnchorCandidatesLocked() const
{
    std::vector<int64_t> candidates;
    if (loadedSample == nullptr)
        return candidates;

    const bool preferWholeFileTransientAnchors = persistentState.legacyLoopBarSelection > 0
        && !(persistentState.transientEditSamples.empty() && loadedSample->analysis.transientSamples.empty());

    if (preferWholeFileTransientAnchors)
    {
        candidates = persistentState.transientEditSamples.empty()
            ? loadedSample->analysis.transientSamples
            : persistentState.transientEditSamples;
    }
    else switch (persistentState.sliceMode)
    {
        case SampleSliceMode::Transient:
            candidates = persistentState.transientEditSamples.empty()
                ? loadedSample->analysis.transientSamples
                : persistentState.transientEditSamples;
            break;
        case SampleSliceMode::Manual:
            candidates.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
                candidates.push_back(cue.samplePosition);
            break;
        case SampleSliceMode::Beat:
            candidates = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples,
                                                 loadedSample->sourceLengthSamples);
            break;
        case SampleSliceMode::Uniform:
        default:
            candidates.reserve(sliceModel.getAllSlices().size());
            for (const auto& slice : sliceModel.getAllSlices())
                candidates.push_back(slice.startSample);
            break;
    }

    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [this](int64_t sample)
                                    {
                                        return sample < 0 || sample >= loadedSample->sourceLengthSamples;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    if (candidates.empty())
        candidates.push_back(0);

    return candidates;
}

void SampleModeEngine::clearRandomVisibleSliceOverrideLocked()
{
    randomVisibleSliceOverrideActive = false;
    randomVisibleSlices.fill(SampleSlice{});
}

bool SampleModeEngine::buildLegacyLoopSyncInfoForSliceIndexLocked(int sliceIndex, LegacyLoopSyncInfo& syncInfo) const
{
    if (loadedSample == nullptr)
        return false;

    const auto& allSlices = sliceModel.getAllSlices();
    if (sliceIndex < 0 || sliceIndex >= static_cast<int>(allSlices.size()))
        return false;

    const int bankIndex = sliceIndex / SliceModel::VisibleSliceCount;
    const int bankStart = bankIndex * SliceModel::VisibleSliceCount;

    syncInfo = {};
    syncInfo.loadedSample = loadedSample;
    syncInfo.visibleBankIndex = bankIndex;
    syncInfo.triggerVisibleSlot = sliceIndex - bankStart;
    syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
        ? persistentState.analyzedTempoBpm
        : loadedSample->analysis.estimatedTempoBpm;
    syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    syncInfo.sliceMode = persistentState.sliceMode;

    if (persistentState.sliceMode == SampleSliceMode::Transient)
    {
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const int sourceIndex = bankStart + slot;
            if (sourceIndex >= 0 && sourceIndex < static_cast<int>(allSlices.size()))
                syncInfo.visibleSlices[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(sourceIndex)];
        }
        syncInfo.markerSamples = persistentState.transientEditSamples.empty()
            ? loadedSample->analysis.transientSamples
            : persistentState.transientEditSamples;
    }
    else if (persistentState.sliceMode == SampleSliceMode::Manual)
    {
        syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
        for (const auto& cue : persistentState.cuePoints)
        {
            if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                syncInfo.markerSamples.push_back(cue.samplePosition);
        }
    }
    else
    {
        syncInfo.markerSamples.reserve(static_cast<size_t>(juce::jmax(0, SliceModel::VisibleSliceCount)));
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const int sourceIndex = bankStart + slot;
            if (sourceIndex >= 0 && sourceIndex < static_cast<int>(allSlices.size()))
            {
                syncInfo.visibleSlices[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(sourceIndex)];
                const auto& slice = allSlices[static_cast<size_t>(sourceIndex)];
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
        }
    }

    syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                            syncInfo.visibleSlices,
                                                            syncInfo.analyzedTempoBpm,
                                                            persistentState.legacyLoopBarSelection);

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
            const auto restoredState = persistentState;
            resetSampleSpecificStateLocked();
            loadedSample = std::move(result.loadedSample);
            persistentState.samplePath = loadedSample->sourcePath;
            const bool restoreExistingEdits = restoredState.samplePath.isNotEmpty()
                && restoredState.samplePath == loadedSample->sourcePath;
            const bool preserveManualTempo = restoreExistingEdits
                && restoredState.analysisSource.startsWithIgnoreCase("manual tempo")
                && restoredState.analyzedTempoBpm > 0.0;
            persistentState.visibleSliceBankIndex = restoreExistingEdits ? restoredState.visibleSliceBankIndex : 0;
            persistentState.viewScroll = restoreExistingEdits ? restoredState.viewScroll : 0.0f;
            persistentState.selectedCueIndex = restoreExistingEdits ? restoredState.selectedCueIndex : -1;
            persistentState.analyzedTempoBpm = preserveManualTempo
                ? restoredState.analyzedTempoBpm
                : loadedSample->analysis.estimatedTempoBpm;
            persistentState.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
            persistentState.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
            persistentState.essentiaUsed = loadedSample->analysis.essentiaUsed;
            persistentState.analysisSource = preserveManualTempo
                ? restoredState.analysisSource
                : loadedSample->analysis.analysisSource;
            if (restoreExistingEdits)
            {
                persistentState.cuePoints = restoredState.cuePoints;
                persistentState.transientEditSamples = restoredState.transientEditSamples;
                persistentState.storedSlices = restoredState.storedSlices;
            }
            rebuildSlicesLocked();
            sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
            pendingTriggerSliceValid = false;
            analysisProgress = 1.0f;
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
            analysisProgress = 0.0f;
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

void SampleModeEngine::handleLoadProgress(int requestId, float progress, const juce::String& statusTextToUse)
{
    bool shouldNotify = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (requestId != activeRequestId)
            return;

        analysisProgress = juce::jlimit(0.0f, 1.0f, progress);
        if (statusTextToUse.isNotEmpty())
            statusText = compactLoadStatusText(statusTextToUse);
        shouldNotify = true;
    }

    if (shouldNotify)
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
    clearRandomVisibleSliceOverrideLocked();
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

void SampleModeComponent::setWaveformColour(juce::Colour colour)
{
    waveformColour = colour;
    repaint();
}

void SampleModeComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(4);
    g.fillAll(juce::Colour(0xff0d1014));

    auto header = bounds.removeFromTop(16);
    prev16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    random16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    next16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    triggerModeArea = header.removeFromLeft(94);
    header.removeFromLeft(4);
    legacyLoopArea = header.removeFromLeft(42);
    header.removeFromLeft(4);
    legacyLoopBarsBounds = header.removeFromLeft(46);
    header.removeFromLeft(4);
    copyLoopBounds = header.removeFromLeft(56);
    header.removeFromLeft(4);
    copyGrainBounds = header.removeFromLeft(60);
    header.removeFromLeft(6);
    tempoHalfBounds = {};
    tempoDoubleBounds = {};
    if (stateSnapshot.estimatedTempoBpm > 0.0 && header.getWidth() >= 44)
    {
        tempoHalfBounds = header.removeFromLeft(18);
        header.removeFromLeft(4);
        tempoDoubleBounds = header.removeFromLeft(18);
        header.removeFromLeft(6);
    }
    auto analysisHeader = header;

    auto drawHeaderButton = [&](juce::Rectangle<int> area, const juce::String& text, bool active)
    {
        g.setColour(active ? juce::Colour(0xff2a3340) : juce::Colour(0xff1a2028));
        g.fillRoundedRectangle(area.toFloat(), 4.0f);
        g.setColour(active ? juce::Colour(0xffd7dce2) : juce::Colour(0xff5a6674));
        g.drawRoundedRectangle(area.toFloat(), 4.0f, 1.0f);
        g.drawFittedText(text, area, juce::Justification::centred, 1);
    };

    drawHeaderButton(prev16Bounds, "Prev 16", stateSnapshot.canNavigateLeft);
    drawHeaderButton(random16Bounds, "Rnd 16", stateSnapshot.visibleSliceBankCount > 1);
    drawHeaderButton(next16Bounds, "Next 16", stateSnapshot.canNavigateRight);
    drawHeaderButton(getTriggerModeButtonBounds(SampleTriggerMode::OneShot), "SHOT", stateSnapshot.triggerMode == SampleTriggerMode::OneShot);
    drawHeaderButton(getTriggerModeButtonBounds(SampleTriggerMode::Loop), "LOOP", stateSnapshot.triggerMode == SampleTriggerMode::Loop);
    drawHeaderButton(getLegacyLoopButtonBounds(), "MLR", stateSnapshot.useLegacyLoopEngine);
    drawHeaderButton(getLegacyLoopBarsButtonBounds(),
                     legacyLoopBarSelectionLabel(stateSnapshot.legacyLoopBarSelection),
                     stateSnapshot.useLegacyLoopEngine || stateSnapshot.legacyLoopBarSelection != 0);
    drawHeaderButton(copyLoopBounds, "To Loop", stateSnapshot.hasSample);
    drawHeaderButton(copyGrainBounds, "To Grain", stateSnapshot.hasSample);
    if (!tempoHalfBounds.isEmpty())
        drawHeaderButton(tempoHalfBounds, "-", stateSnapshot.estimatedTempoBpm > 20.5);
    if (!tempoDoubleBounds.isEmpty())
        drawHeaderButton(tempoDoubleBounds, "+", stateSnapshot.estimatedTempoBpm > 0.0 && stateSnapshot.estimatedTempoBpm < 399.5);

    if (analysisHeader.getWidth() > 48
        && (stateSnapshot.isLoading
            || stateSnapshot.estimatedTempoBpm > 0.0
            || stateSnapshot.estimatedPitchMidi >= 0
            || stateSnapshot.visibleSliceBankCount > 0))
    {
        juce::String analysisText;
        if (stateSnapshot.visibleSliceBankCount > 0)
            analysisText << "Bank " << juce::String(stateSnapshot.visibleSliceBankIndex + 1)
                         << "/" << juce::String(juce::jmax(1, stateSnapshot.visibleSliceBankCount));
        if (stateSnapshot.estimatedTempoBpm > 0.0)
        {
            if (analysisText.isNotEmpty())
                analysisText << "  |  ";
            analysisText << juce::String(stateSnapshot.estimatedTempoBpm, 1) << " BPM";
        }
        if (stateSnapshot.estimatedPitchMidi >= 0)
        {
            if (analysisText.isNotEmpty())
                analysisText << "  |  ";
            analysisText << noteNameForMidi(stateSnapshot.estimatedPitchMidi);
        }
        if (stateSnapshot.analysisSource.isNotEmpty())
        {
            if (analysisText.isNotEmpty())
                analysisText << "  |  ";
            analysisText << analysisBackendDisplayText(stateSnapshot.essentiaUsed, stateSnapshot.analysisSource);
        }
        if (stateSnapshot.isLoading && stateSnapshot.statusText.isNotEmpty())
        {
            if (analysisText.isNotEmpty())
                analysisText << "  |  ";
            analysisText << stateSnapshot.statusText;
        }

        g.setColour(juce::Colour(0xff8d9aab));
        g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
        g.drawFittedText(analysisText, analysisHeader, juce::Justification::centredRight, 1);
    }
    auto progressArea = bounds.removeFromTop(stateSnapshot.isLoading ? 8 : 2);
    if (stateSnapshot.isLoading)
    {
        auto bar = progressArea.reduced(0, 1);
        bar = bar.withWidth(juce::jmax(36, getWidth() - 8));
        g.setColour(juce::Colour(0xff1a2028));
        g.fillRoundedRectangle(bar.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff384657));
        g.drawRoundedRectangle(bar.toFloat(), 3.0f, 1.0f);

        auto fill = bar;
        fill.setWidth(static_cast<int>(std::round(static_cast<float>(bar.getWidth())
            * juce::jlimit(0.0f, 1.0f, stateSnapshot.analysisProgress))));
        g.setColour(waveformColour.withAlpha(0.95f));
        g.fillRoundedRectangle(fill.toFloat(), 3.0f);
    }

    constexpr int footerReserve = 62;
    const int maxWaveformHeight = juce::jmax(80, bounds.getHeight() - footerReserve);
    const int waveformHeight = juce::jmin(juce::jmax(120, maxWaveformHeight),
                                          juce::jmax(80, bounds.getHeight() - 28));
    waveformBounds = bounds.removeFromTop(waveformHeight);
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
        const int64_t visibleSampleStart = static_cast<int64_t>(std::floor(visibleStart * stateSnapshot.totalSamples));
        const int64_t visibleSampleEnd = static_cast<int64_t>(std::ceil(visibleEnd * stateSnapshot.totalSamples));
        const int64_t visibleSampleCount = juce::jmax<int64_t>(1, visibleSampleEnd - visibleSampleStart);
        const bool useDetailedWaveform = stateSnapshot.viewZoom >= kDetailedWaveformZoomThreshold
            && loadedSample->audioBuffer.getNumSamples() > 0
            && visibleSampleCount <= kDetailedWaveformMaxVisibleSamples;

        g.setColour(waveformColour.withAlpha(0.96f));
        if (useDetailedWaveform)
        {
            const auto& buffer = loadedSample->audioBuffer;
            const int channels = juce::jmax(1, buffer.getNumChannels());
            const int waveformWidth = juce::jmax(1, waveformBounds.getWidth());
            for (int pixel = 0; pixel < waveformWidth; ++pixel)
            {
                const float startNorm = static_cast<float>(pixel) / static_cast<float>(waveformWidth);
                const float endNorm = static_cast<float>(pixel + 1) / static_cast<float>(waveformWidth);
                const int64_t sampleStart = visibleSampleStart + static_cast<int64_t>(std::floor(startNorm * visibleSampleCount));
                const int64_t sampleEnd = juce::jmax<int64_t>(
                    sampleStart + 1,
                    visibleSampleStart + static_cast<int64_t>(std::ceil(endNorm * visibleSampleCount)));

                float minValue = 1.0f;
                float maxValue = -1.0f;
                for (int64_t sampleIndex = sampleStart; sampleIndex < sampleEnd; ++sampleIndex)
                {
                    const int clampedIndex = juce::jlimit(0, buffer.getNumSamples() - 1, static_cast<int>(sampleIndex));
                    float mono = 0.0f;
                    for (int ch = 0; ch < channels; ++ch)
                        mono += buffer.getSample(ch, clampedIndex);
                    mono /= static_cast<float>(channels);
                    minValue = juce::jmin(minValue, mono);
                    maxValue = juce::jmax(maxValue, mono);
                }

                if (minValue > maxValue)
                    std::swap(minValue, maxValue);

                const float minY = centerY - (minValue * halfHeight);
                const float maxY = centerY - (maxValue * halfHeight);
                g.drawVerticalLine(waveformBounds.getX() + pixel, juce::jmin(minY, maxY), juce::jmax(minY, maxY));
            }
        }
        else
        {
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
        }

        g.setColour(juce::Colour(0x70ffffff));
        g.drawHorizontalLine(centerY, static_cast<float>(waveformBounds.getX()), static_cast<float>(waveformBounds.getRight()));

        if (const auto loopRange = computeSampleLoopVisualRange(stateSnapshot))
        {
            const float loopStartNorm = juce::jmax(loopRange->getStart(), visibleStart);
            const float loopEndNorm = juce::jmin(loopRange->getEnd(), visibleEnd);
            if (loopEndNorm > loopStartNorm)
            {
                const int loopX = waveformBounds.getX()
                    + juce::roundToInt(((loopStartNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                const int loopEndX = waveformBounds.getX()
                    + juce::roundToInt(((loopEndNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                auto loopRect = juce::Rectangle<int>(loopX,
                                                     waveformBounds.getY() + 2,
                                                     juce::jmax(2, loopEndX - loopX),
                                                     waveformBounds.getHeight() - 4);
                g.setColour(waveformColour.withAlpha(stateSnapshot.useLegacyLoopEngine ? 0.16f : 0.11f));
                g.fillRoundedRectangle(loopRect.toFloat(), 5.0f);
                g.setColour(waveformColour.brighter(0.4f).withAlpha(0.9f));
                g.drawRoundedRectangle(loopRect.toFloat(), 5.0f, 1.2f);
            }
        }

        for (int sliceIndex = 0; sliceIndex < SliceModel::VisibleSliceCount; ++sliceIndex)
        {
            const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(sliceIndex)];
            if (slice.id < 0 || slice.normalizedEnd < visibleStart || slice.normalizedStart > visibleEnd)
                continue;

            const float clippedStart = juce::jmax(slice.normalizedStart, visibleStart);
            const float clippedEnd = juce::jmin(slice.normalizedEnd, visibleEnd);
            if (clippedEnd <= clippedStart)
                continue;

            const int sliceX = waveformBounds.getX()
                + juce::roundToInt(((clippedStart - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            const int sliceEndX = waveformBounds.getX()
                + juce::roundToInt(((clippedEnd - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            auto sliceRect = juce::Rectangle<int>(sliceX,
                                                  waveformBounds.getY() + 4,
                                                  juce::jmax(1, sliceEndX - sliceX),
                                                  waveformBounds.getHeight() - 8);
            const bool isPending = (stateSnapshot.pendingVisibleSliceSlot == sliceIndex);
            const bool isActive = (stateSnapshot.activeVisibleSliceSlot == sliceIndex);
            g.setColour(waveformColour.withAlpha((sliceIndex % 2) == 0 ? 0.08f : 0.04f));
            g.fillRect(sliceRect);

            const int markerX = waveformBounds.getX()
                + juce::roundToInt(((slice.normalizedStart - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            g.setColour((isPending ? juce::Colours::white : waveformColour.brighter(isActive ? 0.55f : 0.28f))
                            .withAlpha(isActive ? 0.98f : 0.94f));
            g.drawVerticalLine(markerX,
                               static_cast<float>(waveformBounds.getY() + 5),
                               static_cast<float>(waveformBounds.getBottom() - 7));

            juce::Path handle;
            handle.startNewSubPath(static_cast<float>(markerX), static_cast<float>(waveformBounds.getY() + 2));
            handle.lineTo(static_cast<float>(markerX - 4), static_cast<float>(waveformBounds.getY() + 8));
            handle.lineTo(static_cast<float>(markerX + 4), static_cast<float>(waveformBounds.getY() + 8));
            handle.closeSubPath();
            g.fillPath(handle);
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

        if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
        {
            for (const auto marker : stateSnapshot.transientMarkers)
            {
                const float markerNorm = safeNormalizedPosition(marker, stateSnapshot.totalSamples);
                if (markerNorm < visibleStart || markerNorm > visibleEnd)
                    continue;

                const int x = waveformBounds.getX()
                    + juce::roundToInt(((markerNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                g.setColour(juce::Colour(0xfff36f6f).withAlpha(0.78f));
                g.drawVerticalLine(x,
                                   static_cast<float>(waveformBounds.getY() + 10),
                                   static_cast<float>(waveformBounds.getBottom() - 12));
            }
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
        const auto emptyText = stateSnapshot.isLoading && stateSnapshot.statusText.isNotEmpty()
            ? stateSnapshot.statusText
            : juce::String("Async loader ready. Click here to load a file.");
        g.drawFittedText(emptyText,
                         waveformBounds.reduced(12),
                         juce::Justification::centred,
                         2);
    }

    auto footer = bounds.reduced(0, 2);
    auto modeRow = footer.removeFromTop(18);
    sliceModeArea = modeRow.removeFromLeft(184);
    footer.removeFromTop(2);
    auto infoBounds = footer.removeFromTop(14);

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

    if (stateSnapshot.hasSample)
    {
        juce::String infoText;
        if (stateSnapshot.visibleSliceBankCount > 0)
            infoText << "Bank " << juce::String(stateSnapshot.visibleSliceBankIndex + 1)
                     << "/" << juce::String(juce::jmax(1, stateSnapshot.visibleSliceBankCount));
        if (stateSnapshot.estimatedTempoBpm > 0.0)
        {
            if (infoText.isNotEmpty())
                infoText << "  |  ";
            infoText << juce::String(stateSnapshot.estimatedTempoBpm, 1) << " BPM";
        }
        if (stateSnapshot.estimatedPitchMidi >= 0)
        {
            if (infoText.isNotEmpty())
                infoText << "  |  ";
            infoText << noteNameForMidi(stateSnapshot.estimatedPitchMidi);
        }
        if (stateSnapshot.analysisSource.isNotEmpty())
        {
            if (infoText.isNotEmpty())
                infoText << "  |  ";
            infoText << analysisBackendDisplayText(stateSnapshot.essentiaUsed, stateSnapshot.analysisSource);
        }
        const auto seconds = stateSnapshot.sourceSampleRate > 0.0
            ? static_cast<double>(stateSnapshot.totalSamples) / stateSnapshot.sourceSampleRate
            : 0.0;
        if (infoText.isNotEmpty())
            infoText << "  |  ";
        infoText << "Zoom " << juce::String(stateSnapshot.viewZoom, 1)
                 << "x  |  " << juce::String(seconds, 2) << " s";
        if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
            infoText << "  |  click add, drag move, shift-drag slice, option-drag MLR by transient, right-click delete";
        else if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
            infoText << "  |  click add, drag move, shift-drag slice freely, option-drag MLR by transient, right-click delete";

        g.drawFittedText(infoText,
                         infoBounds,
                         juce::Justification::centredLeft,
                         1);
    }
}

void SampleModeComponent::resized()
{
}

void SampleModeComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto point = event.getPosition();
    draggingLegacyLoopWindow = false;
    legacyLoopDragStartX = 0;
    legacyLoopDragStepOffset = 0;
    draggingVisibleSliceSlot = -1;
    draggingCueIndex = -1;
    draggingTransientIndex = -1;
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

    if (getLegacyLoopButtonBounds().contains(point))
    {
        if (engine != nullptr)
            engine->setLegacyLoopEngineEnabled(!stateSnapshot.useLegacyLoopEngine);
        return;
    }

    if (getLegacyLoopBarsButtonBounds().contains(point))
    {
        if (onRequestLegacyLoopBarsMenu)
            onRequestLegacyLoopBarsMenu();
        return;
    }

    if (prev16Bounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateLeft)
            onNavigateVisibleBank(-1);
        return;
    }

    if (next16Bounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateRight)
            onNavigateVisibleBank(1);
        return;
    }

    if (random16Bounds.contains(point))
    {
        if (engine != nullptr && stateSnapshot.visibleSliceBankCount > 1)
            engine->randomizeVisibleBank();
        return;
    }

    if (tempoHalfBounds.contains(point))
    {
        if (engine != nullptr)
            engine->scaleAnalyzedTempo(0.5);
        return;
    }

    if (tempoDoubleBounds.contains(point))
    {
        if (engine != nullptr)
            engine->scaleAnalyzedTempo(2.0);
        return;
    }

    if (copyLoopBounds.contains(point))
    {
        if (onCopyToLoop)
            onCopyToLoop();
        return;
    }

    if (copyGrainBounds.contains(point))
    {
        if (onCopyToGrain)
            onCopyToGrain();
        return;
    }

    if (loadedSample == nullptr && waveformBounds.contains(point))
    {
        if (onRequestLoad)
            onRequestLoad();
        return;
    }

    if (engine != nullptr
        && event.mods.isAltDown()
        && stateSnapshot.useLegacyLoopEngine
        && stateSnapshot.legacyLoopBarSelection > 0
        && getVisibleLegacyLoopRangeBounds().contains(point))
    {
        grabKeyboardFocus();
        draggingLegacyLoopWindow = true;
        legacyLoopDragStartX = point.x;
        legacyLoopDragStepOffset = 0;
        return;
    }

    if (engine != nullptr && event.mods.isShiftDown() && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        int visibleSliceSlot = hitTestVisibleSliceMarker(point);
        if (visibleSliceSlot < 0)
            visibleSliceSlot = hitTestVisibleSlice(point);
        if (visibleSliceSlot >= 0)
        {
            if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
            {
                draggingVisibleSliceSlot = visibleSliceSlot;
                return;
            }
            else if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
            {
                draggingCueIndex = findCuePointForVisibleSlice(visibleSliceSlot);
                if (draggingCueIndex < 0)
                    draggingCueIndex = hitTestCuePoint(point);
                if (draggingCueIndex >= 0)
                {
                    engine->selectCuePoint(draggingCueIndex);
                    return;
                }
            }
        }
    }

    if (engine != nullptr && stateSnapshot.sliceMode == SampleSliceMode::Transient && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        const int markerIndex = hitTestTransientMarker(point);
        if (event.mods.isPopupMenu())
        {
            if (markerIndex >= 0)
                engine->deleteTransientMarker(markerIndex);
            return;
        }

        if (markerIndex >= 0)
        {
            draggingTransientIndex = markerIndex;
            return;
        }

        draggingTransientIndex = engine->createTransientMarkerAtNormalizedPosition(normalizedPositionFromPoint(point));
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
    if (engine == nullptr)
        return;

    if (draggingLegacyLoopWindow)
    {
        const int pixelsPerStep = juce::jmax(3, waveformBounds.getWidth() / 256);
        const int stepOffset = (event.getPosition().x - legacyLoopDragStartX) / juce::jmax(1, pixelsPerStep);
        const int deltaSteps = stepOffset - legacyLoopDragStepOffset;
        if (deltaSteps != 0 && engine->nudgeLegacyLoopWindowByAnchorDelta(deltaSteps))
            legacyLoopDragStepOffset = stepOffset;
        return;
    }

    if (draggingVisibleSliceSlot >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Transient)
    {
        engine->moveVisibleSliceToPosition(draggingVisibleSliceSlot,
                                           normalizedPositionFromPoint(event.getPosition()));
        return;
    }

    if (draggingTransientIndex >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Transient)
    {
        draggingTransientIndex = engine->moveTransientMarker(draggingTransientIndex,
                                                             normalizedPositionFromPoint(event.getPosition()));
        return;
    }

    if (draggingCueIndex >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Manual)
        draggingCueIndex = engine->moveCuePoint(draggingCueIndex,
                                                normalizedPositionFromPoint(event.getPosition()));
}

void SampleModeComponent::mouseUp(const juce::MouseEvent&)
{
    draggingLegacyLoopWindow = false;
    legacyLoopDragStartX = 0;
    legacyLoopDragStepOffset = 0;
    draggingVisibleSliceSlot = -1;
    draggingCueIndex = -1;
    draggingTransientIndex = -1;
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
    if (engine == nullptr)
        return false;

    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
            return engine->deleteCuePoint(stateSnapshot.selectedCueIndex);

        if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
        {
            const int transientIndex = hitTestTransientMarker(getMouseXYRelative());
            if (transientIndex >= 0)
                return engine->deleteTransientMarker(transientIndex);
        }
    }

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

int SampleModeComponent::hitTestVisibleSliceMarker(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleRange();
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());

    int bestSlot = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(slot)];
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;
        if (slice.normalizedStart < visibleRange.getStart() || slice.normalizedStart > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((slice.normalizedStart - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kVisibleSliceMarkerHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestSlot = slot;
        }
    }

    return bestSlot;
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

int SampleModeComponent::findCuePointForVisibleSlice(int visibleSlot) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return -1;

    const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < stateSnapshot.cuePoints.size(); ++i)
    {
        const auto& cue = stateSnapshot.cuePoints[i];
        if (cue.samplePosition <= 0)
            continue;

        const int64_t distance = std::abs(cue.samplePosition - slice.startSample);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
            if (distance == 0)
                break;
        }
    }

    return bestIndex;
}

int SampleModeComponent::findTransientMarkerForVisibleSlice(int visibleSlot) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return -1;

    const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < stateSnapshot.transientMarkers.size(); ++i)
    {
        const int64_t markerSample = stateSnapshot.transientMarkers[i];
        if (markerSample <= 0)
            continue;

        const int64_t distance = std::abs(markerSample - slice.startSample);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
            if (distance == 0)
                break;
        }
    }

    return bestIndex;
}

int SampleModeComponent::hitTestTransientMarker(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleRange();
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < stateSnapshot.transientMarkers.size(); ++i)
    {
        const float markerNorm = safeNormalizedPosition(stateSnapshot.transientMarkers[i], stateSnapshot.totalSamples);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kCueHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

juce::Rectangle<int> SampleModeComponent::getVisibleLegacyLoopRangeBounds() const
{
    if (stateSnapshot.totalSamples <= 0 || waveformBounds.isEmpty())
        return {};

    const auto visibleRange = getVisibleRange();
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    if (const auto loopRange = computeSampleLoopVisualRange(stateSnapshot))
    {
        const float loopStartNorm = juce::jmax(loopRange->getStart(), visibleRange.getStart());
        const float loopEndNorm = juce::jmin(loopRange->getEnd(), visibleRange.getEnd());
        if (loopEndNorm > loopStartNorm)
        {
            const int loopX = waveformBounds.getX()
                + juce::roundToInt(((loopStartNorm - visibleRange.getStart()) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            const int loopEndX = waveformBounds.getX()
                + juce::roundToInt(((loopEndNorm - visibleRange.getStart()) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            return juce::Rectangle<int>(loopX,
                                        waveformBounds.getY() + 2,
                                        juce::jmax(2, loopEndX - loopX),
                                        waveformBounds.getHeight() - 4);
        }
    }

    return {};
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
    const int buttonWidth = 45;
    return { triggerModeArea.getX() + (index * (buttonWidth + 4)),
             triggerModeArea.getY(),
             buttonWidth,
             triggerModeArea.getHeight() };
}

juce::Rectangle<int> SampleModeComponent::getLegacyLoopButtonBounds() const
{
    return legacyLoopArea;
}

juce::Rectangle<int> SampleModeComponent::getLegacyLoopBarsButtonBounds() const
{
    return legacyLoopBarsBounds;
}

void SampleModeComponent::timerCallback()
{
    if (engine != nullptr)
        refreshFromEngine();
}
