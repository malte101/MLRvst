// Processes input audio through all filter models and writes output WAV files
// Usage: RunFilters -f input.wav -c 1000 -r 0.5

#include "helpers.hpp"
#include <iostream>
#include <string>
#include <cstdlib>

void PrintHelp(const char* programName) {
    std::cout << "Usage: " << programName << " -f <input.wav> [options]\n\n";
    std::cout << "Processes audio through all filter models and writes output WAV files.\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -f, --file <path>       Input WAV file (required)\n";
    std::cout << "  -c, --cutoff <hz>       Cutoff frequency in Hz (default: 1000.0)\n";
    std::cout << "  -r, --resonance <value> Resonance 0.0-1.0 (default: 0.5)\n";
    std::cout << "  -s, --oversample <n>    Oversampling factor: 0, 2, 4, or 8 (default: 0)\n";
    std::cout << "  -o, --output-dir <dir>  Output directory (default: current directory)\n";
    std::cout << "\n";
    std::cout << "Output files are named: <FilterName>_c<cutoff>_r<resonance>[_os<factor>].wav\n";
    std::cout << "\n";
    std::cout << "Filter Models:\n";
    for (int i = 0; i < static_cast<int>(FilterModel::Count); ++i) {
        std::cout << "  " << i << " - " << FilterModelNames[i] << "\n";
    }
}

std::string GetBasename(const std::string& path) {
    // Find last path separator
    size_t lastSep = path.find_last_of("/\\");
    std::string filename = (lastSep == std::string::npos) ? path : path.substr(lastSep + 1);

    // Remove extension
    size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos) {
        filename = filename.substr(0, lastDot);
    }
    return filename;
}

std::string BuildOutputFilename(
    const std::string& outputDir,
    const std::string& basename,
    const char* filterName,
    float cutoff,
    float resonance,
    int oversampleFactor
) {
    char buffer[512];
    if (oversampleFactor > 0) {
        snprintf(buffer, sizeof(buffer), "%s%s%s_c%.0f_r%.2f_os%dx.wav",
            outputDir.c_str(),
            outputDir.empty() ? "" : "/",
            filterName,
            cutoff,
            resonance,
            oversampleFactor);
    } else {
        snprintf(buffer, sizeof(buffer), "%s%s%s_c%.0f_r%.2f.wav",
            outputDir.c_str(),
            outputDir.empty() ? "" : "/",
            filterName,
            cutoff,
            resonance);
    }
    return std::string(buffer);
}

int main(int argc, char* argv[]) {
    std::string inputFile;
    std::string outputDir;
    float cutoff = 1000.0f;
    float resonance = 0.5f;
    int oversampleFactor = 0;

    // Parse cli args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintHelp(argv[0]);
            return 0;
        }
        else if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            inputFile = argv[++i];
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
        else if ((arg == "-s" || arg == "--oversample") && i + 1 < argc) {
            oversampleFactor = std::atoi(argv[++i]);
            if (oversampleFactor != 0 && oversampleFactor != 2 && oversampleFactor != 4 && oversampleFactor != 8) {
                std::cerr << "Oversampling factor must be 0, 2, 4, or 8.\n";
                return 1;
            }
        }
        else if ((arg == "-o" || arg == "--output-dir") && i + 1 < argc) {
            outputDir = argv[++i];
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            return 1;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: Input file is required. Use -f <input.wav>\n";
        std::cerr << "Use --help for usage information.\n";
        return 1;
    }

    // Load input WAV file
    int sampleRate = 0;
    int numChannels = 0;
    std::vector<float> inputSamples;

    std::cout << "Loading: " << inputFile << "\n";
    if (!ReadWavFile(inputFile.c_str(), sampleRate, numChannels, inputSamples)) {
        std::cerr << "Failed to load WAV file: " << inputFile << "\n";
        return 1;
    }
    std::cout << "Loaded " << inputSamples.size() << " samples, "
              << sampleRate << " Hz, " << numChannels << " channels\n";

    std::string basename = GetBasename(inputFile);

    std::cout << "\nProcessing with cutoff=" << cutoff << " Hz, resonance=" << resonance;
    if (oversampleFactor > 0) {
        std::cout << ", oversampling=" << oversampleFactor << "x";
    }
    std::cout << "\n";
    std::cout << "=========================================\n\n";

    int successCount = 0;
    int filterCount = static_cast<int>(FilterModel::Count);

    for (int i = 0; i < filterCount; ++i) {
        FilterModel model = static_cast<FilterModel>(i);
        const char* filterName = FilterModelNames[i];

        std::cout << "Processing with " << filterName;
        if (oversampleFactor > 0) {
            std::cout << " (" << oversampleFactor << "x)";
        }
        std::cout << "... ";
        std::cout.flush();

        // Create a copy of input samples for this filter
        std::vector<float> samples = inputSamples;

        // Create and configure filter (with or without oversampling)
        std::unique_ptr<LadderFilterBase> filter;
        if (oversampleFactor > 0) {
            MoogLadders::OversamplingPreset preset;
            switch (oversampleFactor) {
                case 2: preset = MoogLadders::OversamplingPreset::X2; break;
                case 4: preset = MoogLadders::OversamplingPreset::X4; break;
                case 8: preset = MoogLadders::OversamplingPreset::X8; break;
                default: preset = MoogLadders::OversamplingPreset::X2; break;
            }
            filter = CreateOversampledFilter(model, static_cast<float>(sampleRate), preset);
        } else {
            filter = CreateFilter(model, static_cast<float>(sampleRate));
        }
        filter->SetCutoff(cutoff);
        filter->SetResonance(resonance);

        // Build output filename
        std::string outputFile = BuildOutputFilename(outputDir, basename, filterName, cutoff, resonance, oversampleFactor);

        // Process
        {
            ScopedTimer t(outputFile);
            filter->Process(samples.data(), static_cast<uint32_t>(samples.size()));
        }


        // Write output
        if (WriteWavFile(outputFile.c_str(), sampleRate, numChannels, samples)) {
            std::cout << "OK -> " << outputFile << "\n";
            successCount++;
        } else {
            std::cout << "FAILED to write " << outputFile << "\n";
        }
    }

    std::cout << "\n=========================================\n";
    std::cout << "Processed " << successCount << "/" << filterCount << " filters successfully.\n";

    return (successCount == filterCount) ? 0 : 1;
}
