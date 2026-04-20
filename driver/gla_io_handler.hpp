#pragma once
#include <aspl/IORequestHandler.hpp>
#include <memory>

class GLAEntityDevice;

// Handles audio I/O for one GLAEntityDevice.
// Reads from the device's ring buffer on the HAL RT thread.
class GLAIOHandler : public aspl::IORequestHandler {
public:
    explicit GLAIOHandler(GLAEntityDevice* device);

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override;

private:
    GLAEntityDevice* _device; // non-owning; device outlives handler
};
