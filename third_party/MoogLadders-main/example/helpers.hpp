#pragma once

#include "StilsonModel.h"
#include "OberheimVariationModel.h"
#include "SimplifiedModel.h"
#include "ImprovedModel.h"
#include "HuovilainenModel.h"
#include "KrajeskiModel.h"
#include "RKSimulationModel.h"
#include "MicrotrackerModel.h"
#include "MusicDSPModel.h"
#include "HyperionModel.h"
#include "LadderFilterOversampledBase.h"

#include <fstream>
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <iostream>

// Filter model identifiers
enum class FilterModel {
    Stilson = 0,
    Simplified,
    Huovilainen,
    Improved,
    Krajeski,
    RKSimulation,
    Microtracker,
    MusicDSP,
    OberheimVariation,
    Hyperion,
    Count
};

static const char* FilterModelNames[] = {
    "Stilson",
    "Simplified",
    "Huovilainen",
    "Improved",
    "Krajeski",
    "RKSimulation",
    "Microtracker",
    "MusicDSP",
    "OberheimVariation",
    "Hyperion"
};

// Oversampling preset identifiers
enum class OversamplePreset {
    None = 0,
    X2,
    X4,
    X8,
    X2_LowLatency,
    X4_LowLatency,
    X8_LowLatency
};

static const char* OversamplePresetNames[] = {
    "none",
    "2x",
    "4x",
    "8x",
    "2x-ll",
    "4x-ll",
    "8x-ll"
};

inline OversamplePreset ParseOversamplePreset(const std::string& str) {
    if (str == "none") return OversamplePreset::None;
    if (str == "2x") return OversamplePreset::X2;
    if (str == "4x") return OversamplePreset::X4;
    if (str == "8x") return OversamplePreset::X8;
    if (str == "2x-ll") return OversamplePreset::X2_LowLatency;
    if (str == "4x-ll") return OversamplePreset::X4_LowLatency;
    if (str == "8x-ll") return OversamplePreset::X8_LowLatency;
    return OversamplePreset::None;
}

inline MoogLadders::OversamplingPreset ToMoogLaddersPreset(OversamplePreset preset) {
    switch (preset) {
        case OversamplePreset::X2: return MoogLadders::OversamplingPreset::X2;
        case OversamplePreset::X4: return MoogLadders::OversamplingPreset::X4;
        case OversamplePreset::X8: return MoogLadders::OversamplingPreset::X8;
        case OversamplePreset::X2_LowLatency: return MoogLadders::OversamplingPreset::X2_LowLatency;
        case OversamplePreset::X4_LowLatency: return MoogLadders::OversamplingPreset::X4_LowLatency;
        case OversamplePreset::X8_LowLatency: return MoogLadders::OversamplingPreset::X8_LowLatency;
        default: return MoogLadders::OversamplingPreset::X2;
    }
}

// Factory function to create the appropriate filter (without oversampling)
inline std::unique_ptr<LadderFilterBase> CreateFilter(FilterModel model, float sampleRate) {
    switch (model) {
        case FilterModel::Stilson: return std::make_unique<StilsonMoog>(sampleRate);
        case FilterModel::Simplified: return std::make_unique<SimplifiedMoog>(sampleRate);
        case FilterModel::Huovilainen: return std::make_unique<HuovilainenMoog>(sampleRate);
        case FilterModel::Improved: return std::make_unique<ImprovedMoog>(sampleRate);
        case FilterModel::Krajeski: return std::make_unique<KrajeskiMoog>(sampleRate);
        case FilterModel::RKSimulation: return std::make_unique<RKSimulationMoog>(sampleRate);
        case FilterModel::Microtracker: return std::make_unique<MicrotrackerMoog>(sampleRate);
        case FilterModel::MusicDSP: return std::make_unique<MusicDSPMoog>(sampleRate);
        case FilterModel::OberheimVariation: return std::make_unique<OberheimVariationMoog>(sampleRate);
        case FilterModel::Hyperion: return std::make_unique<HyperionMoog>(sampleRate);
        default: return std::make_unique<StilsonMoog>(sampleRate);
    }
}

// Factory function to create oversampled filter
inline std::unique_ptr<LadderFilterBase> CreateOversampledFilter(
    FilterModel model,
    float sampleRate,
    MoogLadders::OversamplingPreset preset
) {
    switch (model) {
        case FilterModel::Stilson:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<StilsonMoog>>(sampleRate, preset);
        case FilterModel::Simplified:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<SimplifiedMoog>>(sampleRate, preset);
        case FilterModel::Huovilainen:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<HuovilainenMoog>>(sampleRate, preset);
        case FilterModel::Improved:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<ImprovedMoog>>(sampleRate, preset);
        case FilterModel::Krajeski:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<KrajeskiMoog>>(sampleRate, preset);
        case FilterModel::RKSimulation:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<RKSimulationMoog>>(sampleRate, preset);
        case FilterModel::Microtracker:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<MicrotrackerMoog>>(sampleRate, preset);
        case FilterModel::MusicDSP:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<MusicDSPMoog>>(sampleRate, preset);
        case FilterModel::OberheimVariation:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<OberheimVariationMoog>>(sampleRate, preset);
        case FilterModel::Hyperion:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<HyperionMoog>>(sampleRate, preset);
        default:
            return std::make_unique<MoogLadders::LadderFilterOversampledBase<StilsonMoog>>(sampleRate, preset);
    }
}

// Returns audio as normalized floats (-1.0 to 1.0)
inline bool ReadWavFile(const char* filename, int& sampleRate, int& numChannels, std::vector<float>& samples) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;

    auto read32 = [&]() { uint32_t v; file.read((char*)&v, 4); return v; };
    auto read16 = [&]() { uint16_t v; file.read((char*)&v, 2); return v; };

    // RIFF header
    char riff[4], wave[4];
    file.read(riff, 4); read32(); file.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) || std::memcmp(wave, "WAVE", 4)) return false;

    uint16_t audioFormat = 0, bitsPerSample = 0;
    uint32_t dataSize = 0;
    bool foundFmt = false, foundData = false;

    // Parse chunks
    while (file && !(foundFmt && foundData)) {
        char chunkId[4];
        file.read(chunkId, 4);
        uint32_t chunkSize = read32();
        std::streampos chunkStart = file.tellg();

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            audioFormat = read16();
            numChannels = read16();
            sampleRate = (int)read32();
            read32(); read16(); // byteRate, blockAlign
            bitsPerSample = read16();
            foundFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            foundData = true;
            break; // Stay at data position
        }
        file.seekg(chunkStart + (std::streamoff)chunkSize);
    }

    if (!foundFmt || !foundData || (audioFormat != 1 && audioFormat != 3)) return false;

    // Read and convert samples
    uint32_t numSamples = dataSize / (bitsPerSample / 8);
    samples.resize(numSamples);

    if (audioFormat == 3) { // IEEE float
        file.read((char*)samples.data(), dataSize);
    } else if (bitsPerSample == 16) {
        std::vector<int16_t> raw(numSamples);
        file.read((char*)raw.data(), dataSize);
        for (uint32_t i = 0; i < numSamples; i++) samples[i] = raw[i] / 32768.0f;
    } else if (bitsPerSample == 24) {
        for (uint32_t i = 0; i < numSamples; i++) {
            uint8_t b[3]; file.read((char*)b, 3);
            int32_t v = (b[0] << 8) | (b[1] << 16) | (b[2] << 24);
            samples[i] = (v >> 8) / 8388608.0f;
        }
    } else if (bitsPerSample == 32) {
        std::vector<int32_t> raw(numSamples);
        file.read((char*)raw.data(), dataSize);
        for (uint32_t i = 0; i < numSamples; i++) samples[i] = raw[i] / 2147483648.0f;
    } else if (bitsPerSample == 8) {
        std::vector<uint8_t> raw(numSamples);
        file.read((char*)raw.data(), dataSize);
        for (uint32_t i = 0; i < numSamples; i++) samples[i] = (raw[i] - 128) / 128.0f;
    } else {
        return false;
    }

    return file.good() || file.eof();
}

// Write WAV file (16-bit PCM)
inline bool WriteWavFile(const char* filename, int sampleRate, int numChannels, const std::vector<float>& samples) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;

    auto write32 = [&](uint32_t v) { file.write((char*)&v, 4); };
    auto write16 = [&](uint16_t v) { file.write((char*)&v, 2); };

    uint32_t numSamples = static_cast<uint32_t>(samples.size());
    uint16_t bitsPerSample = 16;
    uint32_t dataSize = numSamples * (bitsPerSample / 8);
    uint32_t fileSize = 36 + dataSize;

    // RIFF header
    file.write("RIFF", 4);
    write32(fileSize);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    write32(16); // chunk size
    write16(1); // audio format (PCM)
    write16(static_cast<uint16_t>(numChannels));
    write32(static_cast<uint32_t>(sampleRate));
    write32(static_cast<uint32_t>(sampleRate * numChannels * (bitsPerSample / 8))); // byte rate
    write16(static_cast<uint16_t>(numChannels * (bitsPerSample / 8))); // block align
    write16(bitsPerSample);

    // data chunk
    file.write("data", 4);
    write32(dataSize);

    // Convert and write samples
    for (uint32_t i = 0; i < numSamples; i++) {
        float clamped = samples[i];
        if (clamped > 1.0f) clamped = 1.0f;
        if (clamped < -1.0f) clamped = -1.0f;
        int16_t sample = static_cast<int16_t>(clamped * 32767.0f);
        file.write((char*)&sample, 2);
    }

    return file.good();
}

class ScopedTimer
{
    std::string message;
    std::chrono::high_resolution_clock::time_point t0;
public:
    ScopedTimer(std::string message) : message{ std::move(message) }, t0{ std::chrono::high_resolution_clock::now() } {}
    ~ScopedTimer()
    {
        const auto timestamp_ms = (std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - t0).count() * 1000);
        std::cout << message << " completed in " << std::to_string(timestamp_ms) << " ms\n";
    }
};

