#pragma once

#include <aspl/IORequestHandler.hpp>
#include <cstring>
#include <memory>
#include <vector>
#include "../common/GLARingBuffer.h"

struct GLAUnifiedIOHandler   : public aspl::IORequestHandler
{
    GLAUnifiedIOHandler (UInt32 n, std::vector<std::unique_ptr<GLARingBuffer>>* r)
        : numChannels (n), rings (r)
    {}

    void OnReadClientInput (const std::shared_ptr<aspl::Client>& /*client*/,
                            const std::shared_ptr<aspl::Stream>& /*stream*/,
                            Float64 /*zeroTimestamp*/,
                            Float64 /*timestamp*/,
                            void* bytes,
                            UInt32 bytesCount) override
    {
        if (numChannels == 0 || !rings)
        {
            std::memset (bytes, 0, bytesCount);
            return;
        }

        const UInt32 frames = bytesCount / (sizeof (float) * numChannels);
        auto out = static_cast<float*> (bytes);

        for (UInt32 ch = 0; ch < numChannels; ++ch)
        {
            if (ch >= rings->size() || !(*rings)[ch])
                continue;

            (*rings)[ch]->read (scratch, frames);

            for (UInt32 f = 0; f < frames; ++f)
                out[f * numChannels + ch] = scratch[f];
        }
    }

private:
    UInt32 numChannels;
    std::vector<std::unique_ptr<GLARingBuffer>>* rings;
    float scratch[4096];
};
