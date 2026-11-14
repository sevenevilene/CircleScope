#pragma once

#include <atomic>
#include <cmath>
#include "RingBuffer.h"
#include "ScopeParams.h"

// Everything a ScopeComponent needs to draw: the two sample rings, the
// sample rate, and the current parameter values. The plugin feeds one from
// its audio callback; a cluster node or wall feeds one per network stream.
struct ScopeSource
{
    // 2^17 samples: ~0.68 s at 192 kHz, more than the render thread ever reads.
    static constexpr size_t ringSize = 1u << 17;

    ScopeSource()
    {
        for (int i = 0; i < scope::numParams; ++i)
        {
            local[i].store (scope::paramTable[i].def);
            param[i] = &local[i];
        }
    }

    float get (int index) const noexcept { return param[index]->load (std::memory_order_relaxed); }

    ScopeRingBuffer rings[2] { ScopeRingBuffer (ringSize), ScopeRingBuffer (ringSize) };
    std::atomic<double> sampleRate { 48000.0 };

    // Real (not normalised) values. param[] points at local[] unless the
    // owner redirects it, as the plugin does to its host parameters.
    std::atomic<float> local[scope::numParams];
    const std::atomic<float>* param[scope::numParams];
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
