#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include "RingBuffer.h"
#include "ScopeParams.h"

// The current values of the parameters in scope::paramTable, real (not
// normalised). param[] points at local[] unless the owner redirects it, as
// the plugin does to its host parameters.
struct ScopeParamSet
{
    ScopeParamSet()
    {
        for (int i = 0; i < scope::numParams; ++i)
        {
            local[i].store (scope::paramTable[i].def);
            param[i].store (&local[i]);
        }
    }

    float get (int index) const noexcept
    {
        return param[index].load (std::memory_order_relaxed)->load (std::memory_order_relaxed);
    }

    // Back to its own storage, keeping the values: for when what param[]
    // was redirected to is about to go away.
    void detach() noexcept
    {
        for (int i = 0; i < scope::numParams; ++i)
        {
            local[i].store (get (i));
            param[i].store (&local[i]);
        }
    }

    std::atomic<float> local[scope::numParams];
    std::atomic<const std::atomic<float>*> param[scope::numParams];
};

// One instance's audio and settings: the two sample rings, the sample rate
// and the parameter values. The plugin feeds one from its audio callback; a
// CurveAnalyser turns it (and the others summed with it) into the curve a
// scope draws.
struct ScopeSource : ScopeParamSet
{
    // 2^17 samples: ~0.68 s at 192 kHz, more than an analyser ever reads.
    static constexpr size_t ringSize = 1u << 17;

    ScopeRingBuffer rings[2] { ScopeRingBuffer (ringSize), ScopeRingBuffer (ringSize) };
    std::atomic<double> sampleRate { 48000.0 };

    // The audio is mono (both rings hold the same): draw one picture, not two.
    std::atomic<bool> mono { false };

    // A clock shared with the other instances in the same host, so their
    // samples can be lined up: the sample at ring position p is at
    // p + clockOffset on the clock. The domain says which clock (see
    // scope::Clock); positions only compare within one domain.
    std::atomic<int64_t> clockOffset { 0 };
    std::atomic<int> clockDomain { 0 };

    // The curve of the Field EQ (see ScopeParams.h). It starts as low
    // pitches at the centre, rising evenly to high ones further out.
    std::atomic<float> fieldCurve[scope::fieldCurvePoints];

    ScopeSource()
    {
        for (int i = 0; i < scope::fieldCurvePoints; ++i)
            fieldCurve[i].store (0.15f + 0.7f * (float) i / (float) (scope::fieldCurvePoints - 1));
    }
};

// Built-in test signal: L = 20 Hz + 60 Hz, R = 20 Hz + 60.5 Hz.
struct TestTone
{
    void render (float* left, float* right, int numSamples, double sampleRate) noexcept
    {
        constexpr double twoPi = 6.283185307179586;
        const double inc20 = twoPi * 20.0 / sampleRate;
        const double inc60 = twoPi * 60.0 / sampleRate;
        const double inc605 = twoPi * 60.5 / sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const float base = 0.7f * (float) std::sin (phase20);
            left[i] = base + 0.3f * (float) std::sin (phase60);
            right[i] = base + 0.3f * (float) std::sin (phase605);
            phase20 = std::fmod (phase20 + inc20, twoPi);
            phase60 = std::fmod (phase60 + inc60, twoPi);
            phase605 = std::fmod (phase605 + inc605, twoPi);
        }
    }

    double phase20 = 0.0, phase60 = 0.0, phase605 = 0.0;
};
