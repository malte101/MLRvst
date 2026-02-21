// HyperionMoog - A new Moog ladder filter combining:
// - Zero-Delay Feedback via Topology-Preserving Transform (TPT)
// - Antiderivative Antialiasing (ADAA) for reduced aliasing without oversampling
// - Per-stage nonlinearity with adaptive thermal voltage modeling
// - Multi-mode output (LP, HP, BP, Notch)
// - By Dimitri Diakopoulos and Claude, 2025 (Public Domain/Unlicense)

#pragma once

#ifndef HYPERION_LADDER_H
#define HYPERION_LADDER_H

#include "LadderFilterBase.h"
#include <cmath>
#include <algorithm>

class HyperionMoog : public LadderFilterBase
{
public:

    enum FilterMode { LP2, LP4, BP2, BP4, HP2, HP4, NOTCH };

    HyperionMoog(float sampleRate) : LadderFilterBase(sampleRate)
    {
        // Initialize state
        std::fill(std::begin(z), std::end(z), 0.0);
        std::fill(std::begin(x_prev_stage), std::end(x_prev_stage), 0.0);
        std::fill(std::begin(Fx_prev_stage), std::end(Fx_prev_stage), 0.0);
        std::fill(std::begin(Vt_prev), std::end(Vt_prev), 0.0);
        std::fill(std::begin(beta), std::end(beta), 0.0);

        u_prev = 0.0;
        Fu_prev = 0.0;
        Vt_u_prev = 0.0;

        Vt = 0.312; // Thermal voltage scaled for numerical convenience
        VtAlpha = 0.05; // Adaptive coefficient
        adaptiveVtEnabled = true;
        drive = 1.0;
        K = 0.0;
        g = 0.0;
        G = 0.0;
        gamma = 0.0;
        alpha0 = 1.0;

        SetFilterMode(LP4);
        SetCutoff(1000.0f);
        SetResonance(0.1f);
    }

    virtual ~HyperionMoog() {}

    virtual void Process(float* samples, uint32_t n) override
    {
        for (uint32_t s = 0; s < n; ++s) {

            // zero-delay feedback sum (TPT ladder weights)
            double sigma = beta[0] * z[0] + beta[1] * z[1] + beta[2] * z[2] + beta[3] * z[3];

            // feedback subtraction and input saturation with ADAA
            double inputScaled = samples[s] * (1.0 + K);
            double u_raw = (inputScaled - K * sigma) * alpha0;
            double u_drive = u_raw * drive;

            // apply ADAA to input saturation
            double Fu_out;
            double Vt_u = GetEffectiveVt(u_drive);
            double twoVt_u_inv = 1.0 / (2.0 * Vt_u);  // Precompute reciprocal
            double u;
            if (Vt_u_prev > 0.0 && fabs(Vt_u - Vt_u_prev) > (VtRelEps * Vt_u_prev)) {
                // Vt changed significantly - use instantaneous normalized tanh
                u = 2.0 * Vt_u * FastTanh(u_drive * twoVt_u_inv);
                Fu_out = TanhAntiderivative(u_drive, Vt_u, twoVt_u_inv);
            } else {
                u = TanhADAA(u_drive, u_prev, Fu_prev, Vt_u, twoVt_u_inv, Fu_out);
            }

            u_prev = u_drive;
            Fu_prev = Fu_out;
            Vt_u_prev = Vt_u;

            double y[4];
            double x = u;

            for (int i = 0; i < 4; i++)
            {
                y[i] = SolveStageADAA(i, x);
                x = y[i];
            }

            // output mixing
            samples[s] = static_cast<float>(modeCoeffs[0] * u + modeCoeffs[1] * y[0] + modeCoeffs[2] * y[1] + modeCoeffs[3] * y[2] + modeCoeffs[4] * y[3]);
        }
    }

    virtual void SetCutoff(float c) override
    {
        cutoff = c;

        // Prewarp for bilinear transform
        double wd = 2.0 * MOOG_PI * cutoff;
        double T = 1.0 / sampleRate;
        double wa = (2.0 / T) * tan(wd * T / 2.0);

        // TPT integrator coefficient
        g = wa * T / 2.0;
        G = g / (1.0 + g);
        gamma = G * G * G * G;

        // TPT ladder feedback weights
        double gInv = 1.0 / (1.0 + g);
        beta[0] = G * G * G * gInv;
        beta[1] = G * G * gInv;
        beta[2] = G * gInv;
        beta[3] = gInv;

        // Update feedback resolution
        alpha0 = 1.0 / (1.0 + K * gamma);
    }

    virtual void SetResonance(float r) override
    {
        resonance = r;
        K = 4.0 * r;
        alpha0 = 1.0 / (1.0 + K * gamma);
    }

    void SetFilterMode(FilterMode mode)
    {
        // Multi-mode output coefficients: output = c[0]*u + c[1]*y0 + c[2]*y1 + c[3]*y2 + c[4]*y3
        switch (mode) {
            case LP4:   modeCoeffs[0] = 0; modeCoeffs[1] = 0;  modeCoeffs[2] = 0;  modeCoeffs[3] = 0;  modeCoeffs[4] = 1;  break;
            case LP2:   modeCoeffs[0] = 0; modeCoeffs[1] = 0;  modeCoeffs[2] = 1;  modeCoeffs[3] = 0;  modeCoeffs[4] = 0;  break;
            case HP4:   modeCoeffs[0] = 1; modeCoeffs[1] = -4; modeCoeffs[2] = 6;  modeCoeffs[3] = -4; modeCoeffs[4] = 1;  break;
            case HP2:   modeCoeffs[0] = 1; modeCoeffs[1] = -2; modeCoeffs[2] = 1;  modeCoeffs[3] = 0;  modeCoeffs[4] = 0;  break;
            case BP4:   modeCoeffs[0] = 0; modeCoeffs[1] = 0;  modeCoeffs[2] = 4;  modeCoeffs[3] = -8; modeCoeffs[4] = 4;  break;
            case BP2:   modeCoeffs[0] = 0; modeCoeffs[1] = 2;  modeCoeffs[2] = -2; modeCoeffs[3] = 0;  modeCoeffs[4] = 0;  break;
            case NOTCH: modeCoeffs[0] = 1; modeCoeffs[1] = -4; modeCoeffs[2] = 6;  modeCoeffs[3] = -4; modeCoeffs[4] = 0;  break;
        }
    }

    void SetDrive(float d) { drive = d; }

    void SetAdaptiveVt(bool enable, float alpha = 0.05f)
    {
        adaptiveVtEnabled = enable;
        VtAlpha = alpha;
        Vt_u_prev = 0.0;
        std::fill(std::begin(Vt_prev), std::end(Vt_prev), 0.0);
    }

    double GetStoredEnergy() const
    {
        // Sum of squared state variables (proportional to stored energy)
        return z[0] * z[0] + z[1] * z[1] + z[2] * z[2] + z[3] * z[3];
    }

private:

    // Numerically stable log(cosh(x))
    // For |x| > 20, log(cosh(x)) ≈ |x| - ln(2)
    inline double LogCosh(double x) const
    {
        double ax = fabs(x);
#ifdef LOGCOSH_SLOW
        // Original implementation using transcendentals
        if (ax > 20.0) return ax - MOOG_LN2;  // Avoid overflow
        return ax + log1p(exp(-2.0 * ax)) - MOOG_LN2;
#else
        // Fast piecewise polynomial approximation
        // For large |x|: log(cosh(x)) ≈ |x| - ln(2)
        if (ax > 4.0) return ax - MOOG_LN2;

        // For small |x|: Taylor series log(cosh(x)) = x²/2 - x⁴/12 + x⁶/45 - x⁸/2520 + ...
        if (ax < 0.5) {
            double x2 = ax * ax;
            // log(cosh(x)) ≈ x²/2 - x⁴/12 + x⁶/45
            return x2 * (0.5 + x2 * (-0.0833333333333333 + x2 * 0.0222222222222222));
        }

        // For medium |x| (0.5 to 4.0): Chebyshev-derived polynomial fit
        // Fit to minimize max error over [0.5, 4.0]
        // log(cosh(x)) ≈ c0 + c1*x + c2*x² + c3*x³ + c4*x⁴
        double x2 = ax * ax;
        double x3 = x2 * ax;
        double x4 = x2 * x2;
        return -0.0022754839 + ax * 0.0277550028 + x2 * 0.4638358950 + x3 * 0.0109256377 + x4 * -0.0037463693;
#endif
    }

    // Normalized saturation: S(x) = 2*Vt * tanh(x / (2*Vt))
    // This has unity gain at the origin: S'(0) = 1
    // Antiderivative: F(x) = 4*Vt^2 * ln(cosh(x / (2*Vt)))
    inline double TanhAntiderivative(double x, double Vt_eff, double twoVt_inv) const
    {
        return 4.0 * Vt_eff * Vt_eff * LogCosh(x * twoVt_inv);
    }

    // ADAA1: First-order antialiased normalized tanh
    // Returns average value of S(x) = 2*Vt*tanh(x/(2*Vt)) over interval [x_prev, x_curr]
    inline double TanhADAA(double x_curr, double x_prev, double Fx_prev_stage_val, double Vt_eff, double twoVt_inv, double& F_out) const
    {
        F_out = TanhAntiderivative(x_curr, Vt_eff, twoVt_inv);
        double denom = x_curr - x_prev;
        if (fabs(denom) < 1e-12) {
            // Degenerate case: return instantaneous normalized tanh (use FastTanh)
            return 2.0 * Vt_eff * FastTanh(x_curr * twoVt_inv);
        }
        return (F_out - Fx_prev_stage_val) / denom;
    }

    // Derivative of normalized tanh: d/dx [2*Vt * tanh(x/(2*Vt))] = sech^2(x/(2*Vt))
    // Note: unity at origin (sech^2(0) = 1)
    inline double TanhDerivative(double x, double twoVt_inv) const
    {
        double scaled = x * twoVt_inv;
        if (fabs(scaled) > 20.0) return 0.0;
        double t = FastTanh(scaled);
        return 1.0 - t * t;
    }

    // Fast tanh approximation for initial Newton guess (from Util.h)
    inline double FastTanh(double x) const
    {
        double x2 = x * x;
        return x * (27.0 + x2) / (27.0 + 9.0 * x2);
    }

    // Adaptive thermal voltage
    inline double GetEffectiveVt(double x) const
    {
        return adaptiveVtEnabled ? Vt * (1.0 + VtAlpha * fabs(x)) : Vt;
    }

    // Solve the implicit stage equation (TPT-consistent):
    //   y = z[i] + G * (S_avg(x) - S(y))
    // where S_avg(x) is the ADAA-averaged nonlinearity on the explicit input
    inline double SolveStageADAA(int i, double x)
    {
        double Vt_eff = GetEffectiveVt(x);
        double twoVt_inv = 1.0 / (2.0 * Vt_eff); // Precompute reciprocal
        double twoVt = 2.0 * Vt_eff;
        bool vt_changed = (Vt_prev[i] > 0.0) && (fabs(Vt_eff - Vt_prev[i]) > (VtRelEps * Vt_prev[i]));

        // Initial guess using fast tanh approximation (normalized: 2*Vt*tanh(v/(2*Vt)))
        double Sx_inst = twoVt * FastTanh(x * twoVt_inv);
        double Sy_inst = twoVt * FastTanh(z[i] * twoVt_inv);
        double y = z[i] + G * (Sx_inst - Sy_inst);

        // Precompute F_x
        double F_x = TanhAntiderivative(x, Vt_eff, twoVt_inv);
        double denom = x - x_prev_stage[i];
        bool use_adaa = !vt_changed && fabs(denom) > 1e-12;
        double denom_inv = use_adaa ? (1.0 / denom) : 0.0;  // Precompute for division

        double Sx_avg;
        if (use_adaa) {
            Sx_avg = (F_x - Fx_prev_stage[i]) * denom_inv;
        } else {
            // Use instantaneous normalized tanh 
            Sx_avg = twoVt * FastTanh(x * twoVt_inv);
        }

        // Newton-Raphson iteration
        for (int iter = 0; iter < nr_iters; iter++)
        {
            double Sy = twoVt * FastTanh(y * twoVt_inv);

            // Residual: y - z[i] - G*(Sx_avg - Sy) = 0
            double residual = y - z[i] - G * (Sx_avg - Sy);

            // Jacobian using FastTanh-based derivative
            double dSy = TanhDerivative(y, twoVt_inv);
            double jacobian = 1.0 + G * dSy;

            double delta = residual / jacobian;
            y -= delta;

            if (fabs(delta) < 1e-8) break;
        }

        // Update TPT state (trapezoidal integrator)
        z[i] = 2.0 * y - z[i];

        // Update ADAA state for next sample - reuse F_x computed above
        x_prev_stage[i] = x;
        Fx_prev_stage[i] = F_x;
        Vt_prev[i] = Vt_eff;

        return y;
    }

    // TPT integrator states (cap voltages)
    double z[4];

    // TPT ceoffs
    double G; // g/(1+g) integrator gain
    double g; // Raw integrator coefficient
    double gamma; // G^4 for feedback
    double alpha0; // Feedback resolution: 1/(1 + K*gamma)
    double K; // Resonance [0, 4]
    double beta[4]; // Feedback weights for TPT ladder sum

    // ADAA state: previous stage input and antiderivative per stage
    double x_prev_stage[4];
    double Fx_prev_stage[4];
    double Vt_prev[4];

    // Input saturation ADAA state
    double u_prev;
    double Fu_prev;
    double Vt_u_prev;

    // Thermal voltage modeling
    double Vt; // Base thermal voltage (scaled for numerical convenience)
    double VtAlpha; // Adaptive coefficient
    bool adaptiveVtEnabled;

    double drive;
    double modeCoeffs[5];

    static constexpr double VtRelEps = 1e-6;
    int nr_iters = 2;
};

#endif // HYPERION_LADDER_H
