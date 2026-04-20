#pragma once
#include <atomic>
#include <cstring>
#include <vector>

// Single-producer single-consumer lock-free ring buffer for float32 audio frames.
// capacity must be a power of two.
class GLARingBuffer {
public:
    explicit GLARingBuffer(size_t capacity)
        : _buf(capacity, 0.0f), _mask(capacity - 1), _head(0), _tail(0) {}

    // Called from producer (USB IOProc thread). Returns frames written.
    size_t write(const float* src, size_t frames) {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t tail = _tail.load(std::memory_order_acquire);
        size_t avail = _buf.size() - (head - tail);
        if (frames > avail) frames = avail;
        for (size_t i = 0; i < frames; ++i)
            _buf[(head + i) & _mask] = src[i];
        _head.store(head + frames, std::memory_order_release);
        return frames;
    }

    // Called from consumer (HAL RT thread). Fills dst with frames, zero-fills remainder.
    void read(float* dst, size_t frames) {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_acquire);
        size_t avail = head - tail;
        size_t toRead = (avail < frames) ? avail : frames;
        for (size_t i = 0; i < toRead; ++i)
            dst[i] = _buf[(tail + i) & _mask];
        if (toRead < frames)
            std::memset(dst + toRead, 0, (frames - toRead) * sizeof(float));
        _tail.store(tail + toRead, std::memory_order_release);
    }

    size_t available() const {
        return _head.load(std::memory_order_acquire) -
               _tail.load(std::memory_order_acquire);
    }

    void reset() {
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
        std::fill(_buf.begin(), _buf.end(), 0.0f);
    }

private:
    std::vector<float> _buf;
    size_t _mask;
    std::atomic<size_t> _head;
    std::atomic<size_t> _tail;
};
