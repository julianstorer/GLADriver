#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Routes audio from positions in a source interleaved buffer to positions in a
// destination interleaved buffer. Knows nothing about CoreAudio, FIFOs, or AVDECC —
// just two float arrays and a list of (src, dst) channel pairs.
struct GLAChannelMatrix
{
    struct Route
    {
        uint8_t src;  // channel index in source interleaved buffer
        uint8_t dst;  // channel index in destination interleaved buffer
    };

    std::vector<Route> routes;
    uint32_t           numDstChannels = 0;

    // Zero dst, then for each route copy src position to dst position.
    // src layout: frame-major interleaved — sample[frame][channel]
    // dst layout: same
    void apply (const float* src, uint32_t numSrcChannels,
                float* dst, uint32_t frames) const noexcept
    {
        std::memset (dst, 0, frames * numDstChannels * sizeof (float));

        for (auto& r : routes)
        {
            if (r.src >= numSrcChannels || r.dst >= numDstChannels)
                continue;

            for (uint32_t f = 0; f < frames; ++f)
                dst[f * numDstChannels + r.dst] = src[f * numSrcChannels + r.src];
        }
    }
};
