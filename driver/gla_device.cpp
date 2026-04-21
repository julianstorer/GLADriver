#include "gla_device.hpp"
#include "gla_io_handler.hpp"
#include <aspl/Stream.hpp>
#include <mach/mach_time.h>
#include <syslog.h>

static constexpr UInt32 sampleRate   = 48000;
static constexpr size_t ringCapacity = 4096; // frames

static aspl::DeviceParameters makeParams (size_t channelCount)
{
    aspl::DeviceParameters p;
    p.Name         = "GreenLight AVB";
    p.Manufacturer = "GreenLight Audio";
    p.DeviceUID    = "com.greenlight.gla-injector.unified";
    p.ModelUID     = "gla-injector-unified";
    p.SampleRate   = sampleRate;
    p.ChannelCount = static_cast<UInt32> (channelCount);
    p.ZeroTimeStampPeriod = GLAUnifiedDevice::zeroTsPeriod;
    p.CanBeDefault = false;
    p.CanBeDefaultForSystemSounds = false;
    return p;
}

GLAUnifiedDevice::GLAUnifiedDevice (std::shared_ptr<const aspl::Context> context,
                                    const std::vector<GLAChannelEntry>& channelEntries)
    : aspl::Device (context, makeParams (channelEntries.size()))
    , entries (channelEntries)
{
    for (size_t i = 0; i < channelEntries.size(); ++i)
    {
        channelToSlot[channelEntries[i].channelIndex] = i;
        rings.push_back (std::make_unique<GLARingBuffer> (ringCapacity));
    }
}

void GLAUnifiedDevice::init()
{
    struct mach_timebase_info tb;
    mach_timebase_info (&tb);
    hostTicksPerFrame = (double (tb.denom) / double (tb.numer)) * 1e9 / sampleRate;

    const UInt32 nChannels = static_cast<UInt32> (entries.size());

    aspl::StreamParameters sp;
    sp.Direction          = aspl::Direction::Input;
    sp.StartingChannel    = 1;
    sp.Format             = {};
    sp.Format.mSampleRate       = sampleRate;
    sp.Format.mFormatID         = kAudioFormatLinearPCM;
    sp.Format.mFormatFlags      = kAudioFormatFlagIsFloat
                                | kAudioFormatFlagIsPacked
                                | kAudioFormatFlagsNativeEndian;
    sp.Format.mChannelsPerFrame = nChannels;
    sp.Format.mBitsPerChannel   = 32;
    sp.Format.mBytesPerFrame    = 4 * nChannels;
    sp.Format.mFramesPerPacket  = 1;
    sp.Format.mBytesPerPacket   = 4 * nChannels;
    AddStreamAsync (sp);

    auto handler = std::make_shared<GLAUnifiedIOHandler> (nChannels, &rings);
    SetIOHandler (handler);
}

GLARingBuffer* GLAUnifiedDevice::getChannelRingBuffer (int channelIndex)
{
    auto it = channelToSlot.find (channelIndex);
    if (it == channelToSlot.end()) return nullptr;
    return rings[it->second].get();
}

OSStatus GLAUnifiedDevice::GetZeroTimeStampImpl (UInt32 /*clientID*/,
                                                  Float64* outSampleTime,
                                                  UInt64*  outHostTime,
                                                  UInt64*  outSeed)
{
    // Anchor to actual current time on first call so the HAL never has to
    // "catch up" from epoch 0 (which causes a tight spin of thousands of cycles).
    UInt64 anchor = anchorHostTime.load (std::memory_order_relaxed);
    if (anchor == 0)
    {
        anchor = mach_absolute_time();
        anchorHostTime.store (anchor, std::memory_order_relaxed);
    }

    const auto period = static_cast<UInt64> (GetZeroTimeStampPeriod());
    const double ticksPerPeriod = hostTicksPerFrame * static_cast<double> (period);
    const UInt64 now = mach_absolute_time();
    UInt64 ctr = periodCounter.load (std::memory_order_relaxed);

    if (now >= anchor + UInt64 (double (ctr + 1) * ticksPerPeriod))
        periodCounter.store (++ctr, std::memory_order_relaxed);

    *outSampleTime = double (ctr) * static_cast<double> (period);
    *outHostTime   = anchor + UInt64 (double (ctr) * ticksPerPeriod);
    *outSeed       = 1;
    return kAudioHardwareNoError;
}

GLAUnifiedDevice::~GLAUnifiedDevice()
{
    syslog (LOG_INFO, "GLA: destroyed unified device (%zu channels)", entries.size());
}
