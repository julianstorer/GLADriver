#pragma once
#include <aspl/IORequestHandler.hpp>
#include <memory>
#include <vector>
#include "../common/gla_ring_buffer.hpp"

// Handles audio I/O for GLAUnifiedDevice.
// The device has one interleaved N-channel stream; this handler reads from
// each channel's ring buffer and interleaves samples into the output buffer.
class GLAUnifiedIOHandler : public aspl::IORequestHandler {
public:
    GLAUnifiedIOHandler(UInt32 nChannels,
                        std::vector<std::unique_ptr<GLARingBuffer>>* rings);

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override;

private:
    UInt32  _nChannels;
    std::vector<std::unique_ptr<GLARingBuffer>>* _rings; // non-owning
    float   _scratch[4096]; // per-channel temp buffer; avoids RT allocation
};
