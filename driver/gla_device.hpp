#pragma once
#include <aspl/Device.hpp>
#include <aspl/IORequestHandler.hpp>
#include <memory>
#include <string>
#include "../common/gla_ring_buffer.hpp"

// One virtual CoreAudio device per AVB entity.
// Device name = entity name (e.g. "Bob's Guitar").
// Has one mono input stream reading from a dedicated ring buffer.
class GLAEntityDevice : public aspl::Device {
public:
    GLAEntityDevice(std::shared_ptr<const aspl::Context> context,
                    const std::string& entityName,
                    uint64_t entityId,
                    int usbChannelIndex);
    ~GLAEntityDevice() override;

    // Must be called once, immediately after make_shared, before AddDevice.
    // (AddStreamAsync uses shared_from_this, which requires the object to
    // already be owned by a shared_ptr.)
    void init();

    GLARingBuffer& ringBuffer() { return _ring; }
    int usbChannelIndex() const { return _usbChannelIndex; }

private:
    GLARingBuffer _ring;
    int _usbChannelIndex;
};
