#pragma once
#include <aspl/Device.hpp>
#include <aspl/IORequestHandler.hpp>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../common/gla_ipc_types.hpp"
#include "../common/gla_ring_buffer.hpp"

// Single CoreAudio device. All AVB sources appear as channels in one interleaved stream.
class GLAUnifiedDevice : public aspl::Device {
public:
    GLAUnifiedDevice(std::shared_ptr<const aspl::Context> context,
                     const std::vector<GLAChannelEntry>& entries);
    ~GLAUnifiedDevice() override;

    // Must be called once after make_shared, before AddDevice.
    void init();

    // Returns the ring buffer for the given USB channel index, or nullptr if unknown.
    GLARingBuffer* getChannelRingBuffer(int channelIndex);

    // Accessed by GLAUnifiedIOHandler (non-owning pointer; device outlives handler).
    std::vector<std::unique_ptr<GLARingBuffer>>& ringBuffers() { return _rings; }

protected:
    // Overridden to anchor the period to the actual current time on first call,
    // preventing the HAL from spinning through thousands of "past" periods before
    // the first StartIO resets the base-class anchor from zero.
    OSStatus GetZeroTimeStampImpl(UInt32 clientID,
                                   Float64* outSampleTime,
                                   UInt64*  outHostTime,
                                   UInt64*  outSeed) override;

private:
    std::vector<GLAChannelEntry> _entries;
    std::unordered_map<int, size_t> _channelToSlot;
    std::vector<std::unique_ptr<GLARingBuffer>> _rings;

    std::atomic<UInt64> _anchorHostTime{0};
    std::atomic<UInt64> _periodCounter{0};
    double _hostTicksPerFrame{0.0};

public:
    static constexpr UInt32 kZeroTsPeriod = 512; // frames per HAL period
};
