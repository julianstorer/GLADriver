#include "gla_io_handler.hpp"
#include <cstring>

GLAUnifiedIOHandler::GLAUnifiedIOHandler (UInt32 n,
                                           std::vector<std::unique_ptr<GLARingBuffer>>* r)
    : numChannels (n), rings (r) {}

void GLAUnifiedIOHandler::OnReadClientInput (const std::shared_ptr<aspl::Client>& /*client*/,
                                              const std::shared_ptr<aspl::Stream>& /*stream*/,
                                              Float64 /*zeroTimestamp*/,
                                              Float64 /*timestamp*/,
                                              void* bytes,
                                              UInt32 bytesCount)
{
    if (numChannels == 0 || !rings)
    {
        std::memset (bytes, 0, bytesCount);
        return;
    }

    const UInt32 frames = bytesCount / (sizeof (float) * numChannels);
    float* out = static_cast<float*> (bytes);

    for (UInt32 ch = 0; ch < numChannels; ++ch)
    {
        if (ch >= rings->size() || !(*rings)[ch]) continue;
        (*rings)[ch]->read (scratch, frames);
        for (UInt32 f = 0; f < frames; ++f)
            out[f * numChannels + ch] = scratch[f];
    }
}
