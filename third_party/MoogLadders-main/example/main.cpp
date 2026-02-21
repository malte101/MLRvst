#if defined(_MSC_VER)
    #pragma comment(lib, "dsound.lib")
#endif

#include "helpers.hpp"
#include "AudioDevice.h"
#include "NoiseGenerator.h"

#include <iostream>

void PrintHelp(const char* programName) {
    std::cout << "Usage: " << programName << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -f, --file <path>       Load WAV file (default: generate white noise)\n";
    std::cout << "  -m, --model <index>     Filter model index (default: 0)\n";
    std::cout << "  -c, --cutoff <hz>       Cutoff frequency in Hz (default: 1000.0)\n";
    std::cout << "  -r, --resonance <value> Resonance 0.0-1.0 (default: 0.5)\n";
    std::cout << "  -o, --oversample <preset> Oversampling preset (default: none)\n";
    std::cout << "  -l, --list-devices      List available audio devices\n";
    std::cout << "\n";
    std::cout << "Filter Models:\n";
    for (int i = 0; i < static_cast<int>(FilterModel::Count); ++i) {
        std::cout << "  " << i << " - " << FilterModelNames[i] << "\n";
    }
    std::cout << "\n";
    std::cout << "Oversampling Presets:\n";
    std::cout << "  none  - No oversampling\n";
    std::cout << "  2x    - 2x quasi-linear phase\n";
    std::cout << "  4x    - 4x quasi-linear phase\n";
    std::cout << "  8x    - 8x quasi-linear phase (highest quality)\n";
    std::cout << "  2x-ll - 2x minimum phase (low latency)\n";
    std::cout << "  4x-ll - 4x minimum phase (low latency)\n";
    std::cout << "  8x-ll - 8x minimum phase (low latency)\n";
}

// e.g. MoogLadderExample -f audio.wav -m 2 -c 2000 -r 0.8 -o 4x
int main(int argc, char* argv[]) {

    std::string wavFile;
    FilterModel filterModel = FilterModel::Stilson;
    float cutoff = 1000.0f;
    float resonance = 0.5f;
    OversamplePreset oversamplePreset = OversamplePreset::None;
    float duration = 3.0f;
    bool listDevices = false;

    // Parse cli args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintHelp(argv[0]);
            return 0;
        }
        else if (arg == "-l" || arg == "--list-devices") {
            listDevices = true;
        }
        else if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            wavFile = argv[++i];
        }
        else if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            int modelIndex = std::atoi(argv[++i]);
            if (modelIndex >= 0 && modelIndex < static_cast<int>(FilterModel::Count)) {
                filterModel = static_cast<FilterModel>(modelIndex);
            } else {
                std::cerr << "Invalid filter model index: " << modelIndex << "\n";
                std::cerr << "Use --help to see available models.\n";
                return 1;
            }
        }
        else if ((arg == "-c" || arg == "--cutoff") && i + 1 < argc) {
            cutoff = static_cast<float>(std::atof(argv[++i]));
            if (cutoff <= 0.0f) {
                std::cerr << "Cutoff (hz) must be positive.\n";
                return 1;
            }
        }
        else if ((arg == "-r" || arg == "--resonance") && i + 1 < argc) {
            resonance = static_cast<float>(std::atof(argv[++i]));
            if (resonance < 0.0f || resonance > 1.0f) {
                std::cerr << "Resonance should be between 0.0 and 1.0.\n";
                return 1;
            }
        }
        else if ((arg == "-o" || arg == "--oversample") && i + 1 < argc) {
            std::string presetStr = argv[++i];
            oversamplePreset = ParseOversamplePreset(presetStr);
            if (presetStr != "none" && oversamplePreset == OversamplePreset::None) {
                std::cerr << "Invalid oversample preset: " << presetStr << "\n";
                std::cerr << "Use --help to see available presets.\n";
                return 1;
            }
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            return 1;
        }
    }

    if (listDevices) {
        AudioDevice::ListAudioDevices();
        return 0;
    }

    // Audio setup
    int sampleRate = 48000;
    int numChannels = 2;
    std::vector<float> samples;

    // Load WAV file or generate noise
    if (!wavFile.empty()) {
        std::cout << "Loading WAV file: " << wavFile << "\n";
        if (!ReadWavFile(wavFile.c_str(), sampleRate, numChannels, samples)) {
            std::cerr << "Failed to load WAV file: " << wavFile << "\n";
            return 1;
        }
        std::cout << "Loaded " << samples.size() << " samples, "
                  << sampleRate << " Hz, " << numChannels << " channels\n";
    } else {
        std::cout << "Generating " << duration << "s of white noise at " << sampleRate << " Hz\n";
        NoiseGenerator gen;
        samples = gen.produce(NoiseGenerator::NoiseType::WHITE, sampleRate, numChannels, duration);
    }

    // Create audio device
    AudioDevice device(numChannels, sampleRate);
    device.Open(device.info.id);

    // Create filter
    std::unique_ptr<LadderFilterBase> filter;

    std::cout << "\nFilter: " << FilterModelNames[static_cast<int>(filterModel)];

    if (oversamplePreset != OversamplePreset::None) {
        auto preset = ToMoogLaddersPreset(oversamplePreset);
        filter = CreateOversampledFilter(filterModel, static_cast<float>(sampleRate), preset);
        std::cout << " (oversampled " << OversamplePresetNames[static_cast<int>(oversamplePreset)] << ")";
    } else {
        filter = CreateFilter(filterModel, static_cast<float>(sampleRate));
    }
    std::cout << "\n";

    // Set filter parameters
    filter->SetCutoff(cutoff);
    filter->SetResonance(resonance);

    filter->Process(samples.data(), static_cast<uint32_t>(samples.size()));
    device.Play(samples);

    return 0;
}
