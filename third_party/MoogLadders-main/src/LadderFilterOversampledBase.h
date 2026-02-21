#pragma once

#ifndef LADDER_FILTER_OVERSAMPLED_BASE_H
#define LADDER_FILTER_OVERSAMPLED_BASE_H

#include "LadderFilterBase.h"
#include "Oversampler.h"
#include <array>
#include <algorithm>
#include <memory>

namespace MoogLadders {

enum class OversamplingPreset
{
    X2,             // 2x, quasi-linear phase (balanced latency/quality)
    X4,             // 4x, quasi-linear phase
    X8,             // 8x, quasi-linear phase (highest quality)
    X2_LowLatency,  // 2x, minimum phase (lowest latency)
    X4_LowLatency,  // 4x, minimum phase
    X8_LowLatency   // 8x, minimum phase
};

// Extract oversampling factor from preset
inline int GetFactorFromPreset(OversamplingPreset preset)
{
    switch (preset)
    {
        case OversamplingPreset::X2:
        case OversamplingPreset::X2_LowLatency:
            return 2;
        case OversamplingPreset::X4:
        case OversamplingPreset::X4_LowLatency:
            return 4;
        case OversamplingPreset::X8:
        case OversamplingPreset::X8_LowLatency:
            return 8;
    }
    return 2;
}

// Extract phase mode from preset
inline Oversampling::PhaseMode GetPhaseMode(OversamplingPreset preset)
{
    switch (preset)
    {
        case OversamplingPreset::X2:
        case OversamplingPreset::X4:
        case OversamplingPreset::X8:
            return Oversampling::PhaseMode::QuasiLinearPhase;
        case OversamplingPreset::X2_LowLatency:
        case OversamplingPreset::X4_LowLatency:
        case OversamplingPreset::X8_LowLatency:
            return Oversampling::PhaseMode::MinimumPhase;
    }
    return Oversampling::PhaseMode::QuasiLinearPhase;
}

// Template wrapper that adds oversampling to any LadderFilterBase-derived filter
//
// Usage:
//   LadderFilterOversampledBase<HuovilainenMoog> filter(44100.0f, OversamplingPreset::X4);
//   filter.SetCutoff(2000.0f);
//   filter.SetResonance(0.8f);
//
//   float buffer[256];
//   filter.Process(buffer, 256);
//
//   // Get latency for compensation
//   uint32_t latency = filter.GetLatency();
//
// The inner filter runs at sampleRate * factor, so filter parameters
// are automatically adjusted. For example, with 4x oversampling at 44.1kHz,
// the inner filter runs at 176.4kHz.
//
template<typename FilterT>
class LadderFilterOversampledBase : public LadderFilterBase
{
public:
    // Maximum buffer size at input rate
    static constexpr uint32_t kMaxBlockSize = Oversampling::kMaxBlockSize;

    // Maximum oversampled buffer size (8x * 4096)
    static constexpr uint32_t kMaxOversampledSize = kMaxBlockSize * 8;

    LadderFilterOversampledBase(float sampleRate, OversamplingPreset preset = OversamplingPreset::X2)
        : LadderFilterBase(sampleRate)
        , preset_(preset)
        , factor_(GetFactorFromPreset(preset))
        , quality_(Oversampling::Quality::Standard)
    {
        // Initialize inner filter at oversampled rate
        float oversampledRate = sampleRate * static_cast<float>(factor_);
        innerFilter_ = std::make_unique<FilterT>(oversampledRate);
        cutoff = 0.0f;
        resonance = 0.0f;

        // Configure oversampler based on preset
        ConfigureOversampler();
    }

    ~LadderFilterOversampledBase() override = default;

    // Process audio samples in-place
    void Process(float* samples, uint32_t n) override
    {
        // Process in chunks to avoid stack overflow
        uint32_t remaining = n;
        uint32_t offset = 0;

        while (remaining > 0)
        {
            uint32_t chunk = std::min(remaining, kMaxBlockSize);
            ProcessChunk(samples + offset, chunk);
            offset += chunk;
            remaining -= chunk;
        }
    }

    // Set cutoff frequency (Hz)
    // This is relative to the original sample rate
    void SetCutoff(float c) override
    {
        cutoff = c;
        innerFilter_->SetCutoff(c);
    }

    // Set resonance (0.0 to 1.0 typically, some filters go higher)
    void SetResonance(float r) override
    {
        resonance = r;
        innerFilter_->SetResonance(r);
    }

    // Change the oversampling preset
    // This will reset the filter state
    void SetPreset(OversamplingPreset preset)
    {
        if (preset == preset_)
            return;

        preset_ = preset;
        int newFactor = GetFactorFromPreset(preset);

        if (newFactor != factor_)
        {
            factor_ = newFactor;

            // Recreate inner filter at new rate
            float oversampledRate = sampleRate * static_cast<float>(factor_);
            innerFilter_ = std::make_unique<FilterT>(oversampledRate);
            innerFilter_->SetCutoff(cutoff);
            innerFilter_->SetResonance(resonance);
        }

        ConfigureOversampler();
        Reset();
    }

    // Set filter quality (affects anti-aliasing filter steepness)
    void SetQuality(Oversampling::Quality quality)
    {
        if (quality == quality_)
            return;

        quality_ = quality;
        ConfigureOversampler();
        Reset();
    }

    // Get latency in samples at the input/output rate
    uint32_t GetLatency() const
    {
        auto phaseMode = GetPhaseMode(preset_);
        return static_cast<uint32_t>(
            Oversampling::GetTotalLatency(factor_, quality_, phaseMode)
        );
    }

    // Get the oversampling factor
    int GetOversamplingFactor() const { return factor_; }

    // Get the current preset
    OversamplingPreset GetPreset() const { return preset_; }

    // Get the current quality
    Oversampling::Quality GetQuality() const { return quality_; }

    // Access the inner filter for advanced configuration
    FilterT& GetInnerFilter() { return *innerFilter_; }
    const FilterT& GetInnerFilter() const { return *innerFilter_; }

    // Reset filter state
    void Reset()
    {
        if (oversampler_) oversampler_->Reset();
    }

private:
    void ConfigureOversampler()
    {
        auto phaseMode = GetPhaseMode(preset_);

        if (!oversampler_ || oversampler_->GetFactor() != factor_)
        {
            oversampler_ = Oversampling::CreateOversampler(factor_);
        }

        oversampler_->SetPhaseMode(phaseMode);
        oversampler_->SetQuality(quality_);
    }

    void ProcessChunk(float* samples, uint32_t n)
    {
        uint32_t oversampledLen = n * factor_;

        // Upsample
        oversampler_->ProcessUp(samples, oversampledBuffer_.data(), n);

        // Process through inner filter at oversampled rate
        innerFilter_->Process(oversampledBuffer_.data(), oversampledLen);

        // Downsample back to original rate
        oversampler_->ProcessDown(oversampledBuffer_.data(), samples, n);
    }

    OversamplingPreset preset_;
    int factor_;
    Oversampling::Quality quality_;

    std::unique_ptr<FilterT> innerFilter_;
    std::unique_ptr<Oversampling::OversamplerBase> oversampler_;
    std::array<float, kMaxOversampledSize> oversampledBuffer_;
};

// Backwards-compatible alias
template<typename FilterT>
using LadderFilterOversampled = LadderFilterOversampledBase<FilterT>;

} // namespace MoogLadders

#endif // LADDER_FILTER_OVERSAMPLED_BASE_H
