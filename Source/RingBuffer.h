#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

// Single-writer (audio thread) / single-reader (render thread) sample ring.
// Fixed size, allocated once, never resized, so the reader can never see a
// reallocation. A visualizer tolerates the rare torn read at the write head.
class ScopeRingBuffer
{
public:
    explicit ScopeRingBuffer (size_t sizePow2) : data (sizePow2, 0.0f), mask (sizePow2 - 1) {}

    void push (const float* samples, int numSamples) noexcept
    {
        auto w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
            data[(w + (size_t) i) & mask] = samples[i];
        writePos.store (w + (size_t) numSamples, std::memory_order_release);
    }

    // Copies the most recent numSamples samples, oldest first.
    void readLatest (float* dest, int numSamples) const noexcept
    {
        const auto start = writePos.load (std::memory_order_acquire) - (size_t) numSamples;
        for (int i = 0; i < numSamples; ++i)
            dest[i] = data[(start + (size_t) i) & mask];
    }

    size_t size() const noexcept { return data.size(); }

private:
    std::vector<float> data;
    const size_t mask;
    std::atomic<size_t> writePos { 0 };
};
