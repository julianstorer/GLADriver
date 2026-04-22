#pragma once
#include <atomic>
#include <cstring>
#include <vector>

// Single-producer single-consumer lock-free ring buffer for float32 audio frames.
// capacity must be a power of two.
struct GLARingBuffer
{
    explicit GLARingBuffer (size_t capacity)
        : buf (capacity, 0.0f), mask (capacity - 1), head (0), tail (0)
    {}

    // Called from producer (USB IOProc thread). Returns frames written.
    size_t write (const float* src, size_t frames)
    {
        auto h = head.load (std::memory_order_relaxed);
        auto t = tail.load (std::memory_order_acquire);
        auto avail = buf.size() - (h - t);

        if (frames > avail)
            frames = avail;

        for (size_t i = 0; i < frames; ++i)
            buf[(h + i) & mask] = src[i];

        head.store (h + frames, std::memory_order_release);
        return frames;
    }

    // Called from consumer (HAL RT thread). Fills dst with frames, zero-fills remainder.
    void read (float* dst, size_t frames)
    {
        auto t = tail.load (std::memory_order_relaxed);
        auto h = head.load (std::memory_order_acquire);
        auto avail  = h - t;
        auto toRead = (avail < frames) ? avail : frames;

        for (size_t i = 0; i < toRead; ++i)
            dst[i] = buf[(t + i) & mask];

        if (toRead < frames)
            std::memset (dst + toRead, 0, (frames - toRead) * sizeof (float));

        tail.store (t + toRead, std::memory_order_release);
    }

    size_t available() const
    {
        return head.load (std::memory_order_acquire) -
               tail.load (std::memory_order_acquire);
    }

    void reset()
    {
        head.store (0, std::memory_order_relaxed);
        tail.store (0, std::memory_order_relaxed);
        std::fill (buf.begin(), buf.end(), 0.0f);
    }

private:
    std::vector<float>   buf;
    size_t               mask;
    std::atomic<size_t>  head;
    std::atomic<size_t>  tail;
};
