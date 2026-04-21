#include "gla_io_handler.hpp"
#include <cstring>

GLAUnifiedIOHandler::GLAUnifiedIOHandler(UInt32 nChannels,
                                          std::vector<std::unique_ptr<GLARingBuffer>>* rings)
    : _nChannels(nChannels), _rings(rings) {}

void GLAUnifiedIOHandler::OnReadClientInput(const std::shared_ptr<aspl::Client>& /*client*/,
                                             const std::shared_ptr<aspl::Stream>& /*stream*/,
                                             Float64 /*zeroTimestamp*/,
                                             Float64 /*timestamp*/,
                                             void* bytes,
                                             UInt32 bytesCount)
{
    if (_nChannels == 0 || !_rings) {
        std::memset(bytes, 0, bytesCount);
        return;
    }

    const UInt32 frames = bytesCount / (sizeof(float) * _nChannels);
    float* out = static_cast<float*>(bytes);

    for (UInt32 ch = 0; ch < _nChannels; ++ch) {
        if (ch >= _rings->size() || !(*_rings)[ch]) continue;
        (*_rings)[ch]->read(_scratch, frames);
        for (UInt32 f = 0; f < frames; ++f)
            out[f * _nChannels + ch] = _scratch[f];
    }
}
