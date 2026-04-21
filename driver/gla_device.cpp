#include "gla_device.hpp"
#include "gla_io_handler.hpp"
#include <aspl/Stream.hpp>
#include <mach/mach_time.h>
#include <syslog.h>

static constexpr UInt32 kSampleRate   = 48000;
static constexpr size_t kRingCapacity = 4096; // frames

static aspl::DeviceParameters makeParams(size_t channelCount) {
    aspl::DeviceParameters p;
    p.Name         = "GreenLight AVB";
    p.Manufacturer = "GreenLight Audio";
    p.DeviceUID    = "com.greenlight.gla-injector.unified";
    p.ModelUID     = "gla-injector-unified";
    p.SampleRate   = kSampleRate;
    p.ChannelCount = static_cast<UInt32>(channelCount);
    p.ZeroTimeStampPeriod = GLAUnifiedDevice::kZeroTsPeriod;
    p.CanBeDefault = false;
    p.CanBeDefaultForSystemSounds = false;
    return p;
}

GLAUnifiedDevice::GLAUnifiedDevice(std::shared_ptr<const aspl::Context> context,
                                    const std::vector<GLAChannelEntry>& entries)
    : aspl::Device(context, makeParams(entries.size()))
    , _entries(entries)
{
    for (size_t i = 0; i < entries.size(); ++i) {
        _channelToSlot[entries[i].channel_index] = i;
        _rings.push_back(std::make_unique<GLARingBuffer>(kRingCapacity));
    }
}

void GLAUnifiedDevice::init()
{
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    _hostTicksPerFrame = (double(tb.denom) / double(tb.numer)) * 1e9 / kSampleRate;

    const UInt32 nChannels = static_cast<UInt32>(_entries.size());

    aspl::StreamParameters sp;
    sp.Direction          = aspl::Direction::Input;
    sp.StartingChannel    = 1;
    sp.Format             = {};
    sp.Format.mSampleRate       = kSampleRate;
    sp.Format.mFormatID         = kAudioFormatLinearPCM;
    sp.Format.mFormatFlags      = kAudioFormatFlagIsFloat
                                | kAudioFormatFlagIsPacked
                                | kAudioFormatFlagsNativeEndian;
    sp.Format.mChannelsPerFrame = nChannels;
    sp.Format.mBitsPerChannel   = 32;
    sp.Format.mBytesPerFrame    = 4 * nChannels;
    sp.Format.mFramesPerPacket  = 1;
    sp.Format.mBytesPerPacket   = 4 * nChannels;
    AddStreamAsync(sp);

    auto handler = std::make_shared<GLAUnifiedIOHandler>(nChannels, &_rings);
    SetIOHandler(handler);
}

GLARingBuffer* GLAUnifiedDevice::getChannelRingBuffer(int channelIndex)
{
    auto it = _channelToSlot.find(channelIndex);
    if (it == _channelToSlot.end()) return nullptr;
    return _rings[it->second].get();
}

OSStatus GLAUnifiedDevice::GetZeroTimeStampImpl(UInt32 /*clientID*/,
                                                 Float64* outSampleTime,
                                                 UInt64*  outHostTime,
                                                 UInt64*  outSeed)
{
    // Anchor to actual current time on first call so the HAL never has to
    // "catch up" from epoch 0 (which causes a tight spin of thousands of cycles).
    UInt64 anchor = _anchorHostTime.load(std::memory_order_relaxed);
    if (anchor == 0) {
        anchor = mach_absolute_time();
        _anchorHostTime.store(anchor, std::memory_order_relaxed);
    }

    // Use the period the HAL property actually reports to avoid silent divergence.
    const auto period = static_cast<UInt64>(GetZeroTimeStampPeriod());
    const double ticksPerPeriod = _hostTicksPerFrame * static_cast<double>(period);
    const UInt64 now = mach_absolute_time();
    UInt64 ctr = _periodCounter.load(std::memory_order_relaxed);

    if (now >= anchor + UInt64(double(ctr + 1) * ticksPerPeriod))
        _periodCounter.store(++ctr, std::memory_order_relaxed);

    *outSampleTime = double(ctr) * static_cast<double>(period);
    *outHostTime   = anchor + UInt64(double(ctr) * ticksPerPeriod);
    *outSeed       = 1;
    return kAudioHardwareNoError;
}

GLAUnifiedDevice::~GLAUnifiedDevice()
{
    syslog(LOG_INFO, "GLA: destroyed unified device (%zu channels)", _entries.size());
}
