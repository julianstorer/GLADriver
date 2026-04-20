#include "gla_device.hpp"
#include "gla_io_handler.hpp"
#include <aspl/Stream.hpp>
#include <syslog.h>

static constexpr UInt32 kSampleRate = 48000;
static constexpr UInt32 kRingCapacity = 4096; // frames

static aspl::DeviceParameters makeParams(const std::string& name, uint64_t entityId) {
    aspl::DeviceParameters p;
    p.Name = name;
    p.Manufacturer = "GreenLight Audio";
    p.DeviceUID = "com.greenlight.gla-injector.entity." + std::to_string(entityId);
    p.ModelUID = "gla-injector-entity";
    p.SampleRate = kSampleRate;
    p.ChannelCount = 1;
    p.CanBeDefault = false;
    p.CanBeDefaultForSystemSounds = false;
    return p;
}

GLAEntityDevice::GLAEntityDevice(std::shared_ptr<const aspl::Context> context,
                                  const std::string& entityName,
                                  uint64_t entityId,
                                  int usbChannelIndex)
    : aspl::Device(context, makeParams(entityName, entityId))
    , _ring(kRingCapacity)
    , _usbChannelIndex(usbChannelIndex)
{
}

void GLAEntityDevice::init()
{
    aspl::StreamParameters sp;
    sp.Direction = aspl::Direction::Input;
    sp.StartingChannel = 1;
    sp.Format = {};
    sp.Format.mSampleRate       = kSampleRate;
    sp.Format.mFormatID         = kAudioFormatLinearPCM;
    sp.Format.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    sp.Format.mChannelsPerFrame = 1;
    sp.Format.mBitsPerChannel   = 32;
    sp.Format.mBytesPerFrame    = 4;
    sp.Format.mFramesPerPacket  = 1;
    sp.Format.mBytesPerPacket   = 4;

    auto stream = AddStreamAsync(sp);
    (void)stream;

    auto handler = std::make_shared<GLAIOHandler>(this);
    SetIOHandler(handler);
}

GLAEntityDevice::~GLAEntityDevice() {
    syslog(LOG_INFO, "GLA: destroyed device (usb ch %d)", _usbChannelIndex);
}
