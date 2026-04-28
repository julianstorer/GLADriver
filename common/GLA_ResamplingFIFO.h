#pragma once

#include "GLA_RingBuffer.h"
#include <cmath>
#include <cstring>

//==============================================================================
// GLALinearResampler — sample-rate conversion algorithm.
//
// Encapsulated separately from the FIFO so the implementation can be swapped
// (e.g. to a polyphase FIR or sinc filter) without touching any pipeline code.
//
// Not thread-safe on its own; called exclusively from the HAL RT thread via
// GLAResamplingFIFO::read().
struct GLALinearResampler
{
    // Sets the resampling ratio and resets all state. Call from a non-RT thread
    // before the first read(), and whenever the source rate changes.
    void setRatio (double srcRate, double dstRate)
    {
        ratio_ = (dstRate > 0.0) ? (srcRate / dstRate) : 1.0;
        reset();
    }

    // Returns the exact number of input samples that must be available (or
    // zero-padded) before calling process() for `numOut` output samples.
    size_t inputNeeded (size_t numOut) const
    {
        return static_cast<size_t> (std::floor (phase_ + static_cast<double> (numOut) * ratio_));
    }

    // Produce `numOut` output samples by linearly interpolating `input[0..inLen-1]`.
    // `inLen` must equal inputNeeded(numOut); the tail may be zero-padded for underrun.
    // Updates phase_ and last_ for the next call.
    void process (const float* input, size_t inLen,
                  float*       output, size_t numOut) noexcept
    {
        size_t inputIdx = 0;

        for (size_t i = 0; i < numOut; ++i)
        {
            const float next = (inputIdx < inLen) ? input[inputIdx] : last_;
            output[i] = last_ + static_cast<float> (phase_) * (next - last_);

            phase_ += ratio_;
            while (phase_ >= 1.0)
            {
                last_  = (inputIdx < inLen) ? input[inputIdx] : last_;
                ++inputIdx;
                phase_ -= 1.0;
            }
        }
    }

    void reset() noexcept { phase_ = 0.0; last_ = 0.0f; }

private:
    double phase_ = 0.0;   // fractional cursor in [0, 1) — double to avoid long-session drift
    double ratio_ = 1.0;   // srcRate / dstRate
    float  last_  = 0.0f;  // left-hand interpolation point carried across chunk boundaries
};


//==============================================================================
// GLAResamplingFIFO — per-channel async resampling bridge.
//
// Wraps a lock-free SPSC input ring and a GLALinearResampler.
// Thread safety: write() is called from the USB IOProc thread (producer);
//                read()  is called from the HAL RT thread (consumer).
//                setSourceRate() / reset() must be called from a non-RT thread
//                before AudioDeviceStart.
struct GLAResamplingFIFO
{
    static constexpr size_t kDefaultRingCapacity = 32768;  // 2^15 — fits 192kHz→48kHz
    static constexpr size_t kScratchCapacity     = 32768;  // pre-allocated; no RT alloc

    // dstRate is the HAL device rate (48000). srcRate may be updated later via
    // setSourceRate() once the USB device rate is known; it defaults to dstRate
    // (passthrough ratio) so the FIFO is safe to use before that call.
    explicit GLAResamplingFIFO (double dstRate,
                                double srcRate  = 48000.0,
                                size_t ringCap  = kDefaultRingCapacity)
        : ring_    (ringCap),
          dstRate_ (dstRate)
    {
        resampler_.setRatio (srcRate, dstRate);
    }

    // Called from a non-RT thread when the USB device's nominal sample rate is known.
    // Resets the ring so stale samples at the old rate are discarded.
    void setSourceRate (double srcRate)
    {
        resampler_.setRatio (srcRate, dstRate_);
        ring_.reset();
    }

    // Producer — USB IOProc thread.
    size_t write (const float* src, size_t frames)
    {
        return ring_.write (src, frames);
    }

    // Consumer — HAL RT thread. Zero-fills dst on underrun; must not allocate.
    void read (float* dst, size_t numOut)
    {
        const size_t consume = resampler_.inputNeeded (numOut);

        // Defensive guard: should never fire with sane numOut (≤4096) and ratio (≤4).
        if (consume > kScratchCapacity)
        {
            std::memset (dst, 0, numOut * sizeof (float));
            return;
        }

        // GLARingBuffer::read() reads min(available, consume) samples and zero-fills
        // the remainder — underrun is handled automatically and gracefully.
        ring_.read (scratch_, consume);

        resampler_.process (scratch_, consume, dst, numOut);
    }

    void reset()
    {
        ring_.reset();
        resampler_.reset();
    }

    size_t available() const { return ring_.available(); }

private:
    GLARingBuffer      ring_;
    GLALinearResampler resampler_;
    double             dstRate_;
    float              scratch_[kScratchCapacity];  // read-side staging; plain array, no heap
};
