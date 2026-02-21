#pragma once

#ifndef OVERSAMPLER_H
#define OVERSAMPLER_H

#include "HalfBandFilter.h"
#include <array>
#include <cstdint>
#include <algorithm>
#include <memory>

namespace MoogLadders {
namespace Oversampling {

// Maximum supported block size at the input rate
static constexpr uint32_t kMaxBlockSize = 4096;

// Abstract base class for oversamplers to enable runtime polymorphism
class OversamplerBase
{
public:
    virtual ~OversamplerBase() = default;

    virtual void SetPhaseMode(PhaseMode mode) = 0;
    virtual void SetQuality(Quality quality) = 0;
    virtual void Reset() = 0;

    virtual void ProcessUp(const float* in, float* out, uint32_t inLen) = 0;
    virtual void ProcessDown(const float* in, float* out, uint32_t outLen) = 0;

    virtual int GetFactor() const = 0;
    virtual int GetLatency() const = 0;
    virtual PhaseMode GetPhaseMode() const = 0;
    virtual Quality GetQuality() const = 0;
};

// Pre-allocated buffer storage for intermediate stages
// Sized for maximum 8x oversampling
template<int Factor>
struct OversamplingBuffers
{
    static constexpr int kMaxOversampledSize = kMaxBlockSize * Factor;

    // Intermediate buffers for cascaded stages
    // For 2x: 1 stage, no intermediate buffers needed
    // For 4x: 2 stages, 1 intermediate buffer (2x size)
    // For 8x: 3 stages, 2 intermediate buffers (2x and 4x size)
    std::array<float, kMaxOversampledSize> stage0;  // Full oversampled rate
    std::array<float, kMaxOversampledSize / 2> stage1;  // Half of max
    std::array<float, kMaxOversampledSize / 4> stage2;  // Quarter of max
};

// Compile-time calculation of number of stages
constexpr int NumStagesForFactor(int factor)
{
    return (factor == 2) ? 1 : (factor == 4) ? 2 : 3;
}

// Oversampler with cascaded half-band filter stages
//
// Template parameter Factor must be 2, 4, or 8
//
// For Factor=2: Uses 1 half-band stage (2x up, 2x down)
// For Factor=4: Uses 2 half-band stages (2x -> 2x up, 2x -> 2x down)
// For Factor=8: Uses 3 half-band stages (2x -> 2x -> 2x up, 2x -> 2x -> 2x down)
//
// Example usage:
//   Oversampler<4> os;  // 4x oversampling
//   os.SetPhaseMode(PhaseMode::QuasiLinearPhase);
//   os.SetQuality(Quality::Standard);
//
//   float input[256];
//   float oversampled[1024];  // 256 * 4
//   float output[256];
//
//   os.ProcessUp(input, oversampled, 256);
//   // ... process at oversampled rate ...
//   os.ProcessDown(oversampled, output, 256);
//
template<int Factor>
class Oversampler : public OversamplerBase
{
    static_assert(Factor == 2 || Factor == 4 || Factor == 8,
                  "Oversampling factor must be 2, 4, or 8");

public:
    static constexpr int kNumStages = NumStagesForFactor(Factor);
    static constexpr int kFactor = Factor;

    Oversampler()
        : phaseMode_(PhaseMode::QuasiLinearPhase)
        , quality_(Quality::Standard)
    {
        ConfigureStages();
    }

    void SetPhaseMode(PhaseMode mode) override
    {
        if (mode != phaseMode_)
        {
            phaseMode_ = mode;
            ConfigureStages();
        }
    }

    void SetQuality(Quality quality) override
    {
        if (quality != quality_)
        {
            quality_ = quality;
            ConfigureStages();
        }
    }

    void Reset() override
    {
        for (int i = 0; i < kNumStages; ++i)
        {
            upStages_[i].Reset();
            downStages_[i].Reset();
        }
    }

    // Upsample from input rate to oversampled rate
    // in: input buffer of length inLen
    // out: output buffer of length inLen * Factor
    void ProcessUp(const float* in, float* out, uint32_t inLen) override
    {
        if (inLen > kMaxBlockSize)
        {
            // Process in chunks if input exceeds max block size
            uint32_t remaining = inLen;
            uint32_t offset = 0;
            while (remaining > 0)
            {
                uint32_t chunk = std::min(remaining, kMaxBlockSize);
                ProcessUp(in + offset, out + offset * Factor, chunk);
                offset += chunk;
                remaining -= chunk;
            }
            return;
        }

        if constexpr (Factor == 2)
        {
            // Single stage: input -> 2x output
            upStages_[0].Upsample2x(in, out, inLen);
        }
        else if constexpr (Factor == 4)
        {
            // Two stages: input -> 2x -> 4x output
            upStages_[0].Upsample2x(in, buffers_.stage1.data(), inLen);
            upStages_[1].Upsample2x(buffers_.stage1.data(), out, inLen * 2);
        }
        else // Factor == 8
        {
            // Three stages: input -> 2x -> 4x -> 8x output
            upStages_[0].Upsample2x(in, buffers_.stage2.data(), inLen);
            upStages_[1].Upsample2x(buffers_.stage2.data(), buffers_.stage1.data(), inLen * 2);
            upStages_[2].Upsample2x(buffers_.stage1.data(), out, inLen * 4);
        }
    }

    // Downsample from oversampled rate to output rate
    // in: input buffer of length outLen * Factor
    // out: output buffer of length outLen
    void ProcessDown(const float* in, float* out, uint32_t outLen) override
    {
        if (outLen > kMaxBlockSize)
        {
            // Process in chunks if output exceeds max block size
            uint32_t remaining = outLen;
            uint32_t offset = 0;
            while (remaining > 0)
            {
                uint32_t chunk = std::min(remaining, kMaxBlockSize);
                ProcessDown(in + offset * Factor, out + offset, chunk);
                offset += chunk;
                remaining -= chunk;
            }
            return;
        }

        if constexpr (Factor == 2)
        {
            // Single stage: 2x input -> output
            downStages_[0].Downsample2x(in, out, outLen);
        }
        else if constexpr (Factor == 4)
        {
            // Two stages: 4x input -> 2x -> output
            // Note: stages are in reverse order for downsampling
            downStages_[1].Downsample2x(in, buffers_.stage1.data(), outLen * 2);
            downStages_[0].Downsample2x(buffers_.stage1.data(), out, outLen);
        }
        else // Factor == 8
        {
            // Three stages: 8x input -> 4x -> 2x -> output
            downStages_[2].Downsample2x(in, buffers_.stage1.data(), outLen * 4);
            downStages_[1].Downsample2x(buffers_.stage1.data(), buffers_.stage2.data(), outLen * 2);
            downStages_[0].Downsample2x(buffers_.stage2.data(), out, outLen);
        }
    }

    // Get total latency in samples at the input/output rate
    int GetLatency() const override
    {
        return GetTotalLatency(Factor, quality_, phaseMode_);
    }

    int GetFactor() const override { return Factor; }
    PhaseMode GetPhaseMode() const override { return phaseMode_; }
    Quality GetQuality() const override { return quality_; }

private:
    void ConfigureStages()
    {
        for (int i = 0; i < kNumStages; ++i)
        {
            upStages_[i].SetMode(phaseMode_, quality_);
            downStages_[i].SetMode(phaseMode_, quality_);
        }
    }

    PhaseMode phaseMode_;
    Quality quality_;
    std::array<HalfBandFilter, kNumStages> upStages_;
    std::array<HalfBandFilter, kNumStages> downStages_;
    OversamplingBuffers<Factor> buffers_;
};

// Type aliases for common oversampling factors
using Oversampler2x = Oversampler<2>;
using Oversampler4x = Oversampler<4>;
using Oversampler8x = Oversampler<8>;

// Factory function to create an oversampler with the specified factor
inline std::unique_ptr<OversamplerBase> CreateOversampler(int factor)
{
    switch (factor)
    {
        case 2: return std::make_unique<Oversampler2x>();
        case 4: return std::make_unique<Oversampler4x>();
        case 8: return std::make_unique<Oversampler8x>();
        default: return std::make_unique<Oversampler2x>();
    }
}

} // namespace Oversampling
} // namespace MoogLadders

#endif // OVERSAMPLER_H
