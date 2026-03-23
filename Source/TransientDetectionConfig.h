#pragma once

#include <array>
#include <atomic>

#include <juce_core/juce_core.h>

enum class TransientOnsetMethod
{
    Hybrid = 0,
    Hfc,
    SpecFlux
};

namespace TransientDetectionConfig
{
inline std::atomic<int> gTransientOnsetMethodChoice{static_cast<int>(TransientOnsetMethod::SpecFlux)};
inline std::atomic<int> gTransientSensitivityChoice{2};
inline std::atomic<int> gTransientSnapChoice{3};
inline std::atomic<int> gTransientSpacingChoice{2};

inline TransientOnsetMethod sanitizedTransientOnsetMethodChoice(int rawChoice) noexcept
{
    return static_cast<TransientOnsetMethod>(juce::jlimit(0,
                                                          static_cast<int>(TransientOnsetMethod::SpecFlux),
                                                          rawChoice));
}

inline float transientChoiceToNormalized01(int rawChoice) noexcept
{
    return juce::jlimit(0.0f, 1.0f, static_cast<float>(juce::jlimit(0, 4, rawChoice)) / 4.0f);
}

inline void setConfig(int onsetMethodChoice,
                      int sensitivityChoice,
                      int snapChoice,
                      int spacingChoice) noexcept
{
    gTransientOnsetMethodChoice.store(static_cast<int>(sanitizedTransientOnsetMethodChoice(onsetMethodChoice)),
                                      std::memory_order_release);
    gTransientSensitivityChoice.store(juce::jlimit(0, 4, sensitivityChoice), std::memory_order_release);
    gTransientSnapChoice.store(juce::jlimit(0, 4, snapChoice), std::memory_order_release);
    gTransientSpacingChoice.store(juce::jlimit(0, 4, spacingChoice), std::memory_order_release);
}

inline float configuredTransientSensitivity01() noexcept
{
    return transientChoiceToNormalized01(gTransientSensitivityChoice.load(std::memory_order_acquire));
}

inline float configuredTransientSnap01() noexcept
{
    return transientChoiceToNormalized01(gTransientSnapChoice.load(std::memory_order_acquire));
}

inline float configuredTransientSpacingScale() noexcept
{
    static constexpr std::array<float, 5> kSpacingScales{{0.70f, 0.85f, 1.0f, 1.2f, 1.4f}};
    return kSpacingScales[static_cast<size_t>(juce::jlimit(0, 4, gTransientSpacingChoice.load(std::memory_order_acquire)))];
}

inline TransientOnsetMethod configuredTransientOnsetMethod() noexcept
{
    return sanitizedTransientOnsetMethodChoice(gTransientOnsetMethodChoice.load(std::memory_order_acquire));
}
} // namespace TransientDetectionConfig
