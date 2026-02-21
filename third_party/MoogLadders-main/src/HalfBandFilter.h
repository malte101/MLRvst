#pragma once

#ifndef HALFBAND_FILTER_H
#define HALFBAND_FILTER_H

#include "OversamplingFilterCoeffs.hpp"
#include <cstring>
#include <algorithm>

namespace MoogLadders {
namespace Oversampling {

// First-order allpass section
// Transfer function: H(z) = (a1 + z^-1) / (1 + a1*z^-1)
struct AllpassFirstOrder
{
    double coeff = 0.0;
    double state = 0.0;

    void SetCoeff(double a1)
    {
        coeff = a1;
    }

    void Reset()
    {
        state = 0.0;
    }

    double Process(double x)
    {
        double y = coeff * x + state;
        state = x - coeff * y;
        return y;
    }
};

// Second-order allpass section
// Transfer function: H(z) = (a2 + a1*z^-1 + z^-2) / (1 + a1*z^-1 + a2*z^-2)
struct AllpassSecondOrder
{
    double a1 = 0.0;
    double a2 = 0.0;
    double w0 = 0.0;
    double w1 = 0.0;

    void SetCoeffs(double c1, double c2)
    {
        a1 = c1;
        a2 = c2;
    }

    void Reset()
    {
        w0 = 0.0;
        w1 = 0.0;
    }

    double Process(double x)
    {
        // Direct Form II transposed
        double w = x - a1 * w0 - a2 * w1;
        double y = a2 * w + a1 * w0 + w1;
        w1 = w0;
        w0 = w;
        return y;
    }
};

// Chain of allpass sections (variable length, up to 4 sections max)
class AllpassChain
{
public:
    static constexpr int kMaxSections = 4;

    AllpassChain() : numFirstOrder_(0), numSecondOrder_(0) {}

    void Configure(const AllpassCoeff* coeffs, uint32_t count)
    {
        numFirstOrder_ = 0;
        numSecondOrder_ = 0;

        for (uint32_t i = 0; i < count && i < kMaxSections; ++i)
        {
            if (coeffs[i].order == 1)
            {
                firstOrder_[numFirstOrder_].SetCoeff(coeffs[i].a1);
                sectionOrder_[i] = 1;
                sectionIndex_[i] = numFirstOrder_;
                numFirstOrder_++;
            }
            else
            {
                secondOrder_[numSecondOrder_].SetCoeffs(coeffs[i].a1, coeffs[i].a2);
                sectionOrder_[i] = 2;
                sectionIndex_[i] = numSecondOrder_;
                numSecondOrder_++;
            }
        }
        totalSections_ = std::min(count, static_cast<uint32_t>(kMaxSections));
    }

    void Reset()
    {
        for (int i = 0; i < numFirstOrder_; ++i)
            firstOrder_[i].Reset();
        for (int i = 0; i < numSecondOrder_; ++i)
            secondOrder_[i].Reset();
    }

    double Process(double x)
    {
        double y = x;

        // Process sections in order
        int fo_idx = 0;
        int so_idx = 0;

        for (uint32_t i = 0; i < totalSections_; ++i)
        {
            if (sectionOrder_[i] == 1)
            {
                y = firstOrder_[fo_idx++].Process(y);
            }
            else
            {
                y = secondOrder_[so_idx++].Process(y);
            }
        }

        return y;
    }

private:
    AllpassFirstOrder firstOrder_[kMaxSections];
    AllpassSecondOrder secondOrder_[kMaxSections];
    int numFirstOrder_;
    int numSecondOrder_;
    uint32_t totalSections_ = 0;
    int sectionOrder_[kMaxSections] = {0};
    int sectionIndex_[kMaxSections] = {0};
};

// Half-band IIR filter using polyphase allpass decomposition
//
// A half-band filter H(z) decomposes into:
//   H(z) = 0.5 * [A0(z^2) + z^-1 * A1(z^2)]
//
// Where A0 and A1 are allpass filters. This structure allows
// each allpass branch to operate at half the rate, halving
// the computational cost.
//
// For upsampling by 2:
//   - Insert zeros between samples
//   - Apply the half-band lowpass to remove imaging
//
// For downsampling by 2:
//   - Apply the half-band lowpass to remove aliasing
//   - Discard every other sample
class HalfBandFilter
{
public:
    HalfBandFilter()
        : phaseMode_(PhaseMode::QuasiLinearPhase)
        , quality_(Quality::Standard)
        , prevBranch1_(0.0)
        , latency_(0)
    {
        Configure(phaseMode_, quality_);
    }

    void SetMode(PhaseMode mode, Quality quality)
    {
        if (mode != phaseMode_ || quality != quality_)
        {
            phaseMode_ = mode;
            quality_ = quality;
            Configure(mode, quality);
        }
    }

    void Reset()
    {
        branch0_.Reset();
        branch1_.Reset();
        prevBranch1_ = 0.0;
    }

    // 2x upsampling: 1 input sample -> 2 output samples
    // Uses polyphase structure for efficiency
    void Upsample2x(double input, double& out0, double& out1)
    {
        // For upsampling, we conceptually insert a zero between samples,
        // then filter. With polyphase decomposition:
        //
        // Even output (out0): from branch0 processing input
        // Odd output (out1): from branch1 processing input
        //
        // The polyphase structure means each branch only needs to
        // process the actual input samples, not the zeros.

        double b0 = branch0_.Process(input);
        double b1 = branch1_.Process(input);

        // Recombine with proper phase
        // H(z) = 0.5 * [A0(z^2) + z^-1 * A1(z^2)]
        out0 = 0.5 * (b0 + prevBranch1_);
        out1 = 0.5 * (b0 - prevBranch1_);

        prevBranch1_ = b1;
    }

    // 2x downsampling: 2 input samples -> 1 output sample
    // Uses polyphase structure for efficiency
    double Downsample2x(double in0, double in1)
    {
        // For downsampling, we filter first then decimate.
        // With polyphase decomposition, we only process every other sample
        // through each branch, achieving the same result at half the cost.

        // Even samples go through branch0
        double b0 = branch0_.Process(in0);

        // Odd samples go through branch1
        double b1 = branch1_.Process(in1);

        // Recombine
        return 0.5 * (b0 + b1);
    }

    // Buffer-based upsampling for efficiency
    // in: input buffer of length inLen
    // out: output buffer of length inLen * 2
    void Upsample2x(const float* in, float* out, uint32_t inLen)
    {
        for (uint32_t i = 0; i < inLen; ++i)
        {
            double o0, o1;
            Upsample2x(static_cast<double>(in[i]), o0, o1);
            out[i * 2] = static_cast<float>(o0);
            out[i * 2 + 1] = static_cast<float>(o1);
        }
    }

    // Buffer-based downsampling for efficiency
    // in: input buffer of length outLen * 2
    // out: output buffer of length outLen
    void Downsample2x(const float* in, float* out, uint32_t outLen)
    {
        for (uint32_t i = 0; i < outLen; ++i)
        {
            double result = Downsample2x(
                static_cast<double>(in[i * 2]),
                static_cast<double>(in[i * 2 + 1])
            );
            out[i] = static_cast<float>(result);
        }
    }

    // Get filter latency in samples (at the filter's operating rate)
    int GetLatency() const
    {
        return latency_;
    }

    PhaseMode GetPhaseMode() const { return phaseMode_; }
    Quality GetQuality() const { return quality_; }

private:
    void Configure(PhaseMode mode, Quality quality)
    {
        HalfBandCoeffs coeffs = GetHalfBandCoeffs(quality, mode);

        branch0_.Configure(coeffs.branch0, coeffs.branch0_count);
        branch1_.Configure(coeffs.branch1, coeffs.branch1_count);

        latency_ = coeffs.latency_samples;

        Reset();
    }

    PhaseMode phaseMode_;
    Quality quality_;
    AllpassChain branch0_;
    AllpassChain branch1_;
    double prevBranch1_;
    int latency_;
};

} // namespace Oversampling
} // namespace MoogLadders

#endif // HALFBAND_FILTER_H
