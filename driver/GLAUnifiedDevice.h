#pragma once
#include <aspl/Device.hpp>
#include <aspl/IORequestHandler.hpp>
#include <aspl/Stream.hpp>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mach/mach_time.h>
#include <syslog.h>
#include "../common/GLAIPCTypes.h"
#include "../common/GLARingBuffer.h"
#include "GLAUnifiedIOHandler.h"

// Single CoreAudio device. All AVB sources appear as channels in one interleaved stream.
class GLAUnifiedDevice : public aspl::Device
{
public:
    GLAUnifiedDevice (std::shared_ptr<const aspl::Context> context,
                      const std::vector<GLAChannelEntry>& channelEntries)
        : aspl::Device (context, makeParams_ (channelEntries.size()))
        , entries (channelEntries)
    {
        for (size_t i = 0; i < channelEntries.size(); ++i)
        {
            channelToSlot[channelEntries[i].channelIndex] = i;
            rings.push_back (std::make_unique<GLARingBuffer> (ringCapacity_));
        }
    }

    ~GLAUnifiedDevice() override
    {
        syslog (LOG_INFO, "GLA: destroyed unified device (%zu channels)", entries.size());
    }

    void init()
    {
        struct mach_timebase_info tb;
        mach_timebase_info (&tb);
        hostTicksPerFrame = (double (tb.denom) / double (tb.numer)) * 1e9 / sampleRate_;

        const UInt32 nChannels = static_cast<UInt32> (entries.size());

        aspl::StreamParameters sp;
        sp.Direction          = aspl::Direction::Input;
        sp.StartingChannel    = 1;
        sp.Format             = {};
        sp.Format.mSampleRate       = sampleRate_;
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

    GLARingBuffer* getChannelRingBuffer (int channelIndex)
    {
        auto it = channelToSlot.find (channelIndex);
        if (it == channelToSlot.end()) return nullptr;
        return rings[it->second].get();
    }

    std::vector<std::unique_ptr<GLARingBuffer>>& ringBuffers() { return rings; }

    static constexpr UInt32 zeroTsPeriod = 512;

protected:
    OSStatus GetZeroTimeStampImpl (UInt32 /*clientID*/,
                                   Float64* outSampleTime,
                                   UInt64*  outHostTime,
                                   UInt64*  outSeed) override
    {
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

private:
    static constexpr UInt32 sampleRate_   = 48000;
    static constexpr size_t ringCapacity_ = 4096;

    static aspl::DeviceParameters makeParams_ (size_t channelCount)
    {
        aspl::DeviceParameters p;
        p.Name         = "GreenLight AVB";
        p.Manufacturer = "GreenLight Audio";
        p.DeviceUID    = "com.greenlight.gla-injector.unified";
        p.ModelUID     = "gla-injector-unified";
        p.SampleRate   = sampleRate_;
        p.ChannelCount = static_cast<UInt32> (channelCount);
        p.ZeroTimeStampPeriod = zeroTsPeriod;
        p.CanBeDefault = false;
        p.CanBeDefaultForSystemSounds = false;
        return p;
    }

    std::vector<GLAChannelEntry>                 entries;
    std::unordered_map<int, size_t>              channelToSlot;
    std::vector<std::unique_ptr<GLARingBuffer>>  rings;

    std::atomic<UInt64> anchorHostTime { 0 };
    std::atomic<UInt64> periodCounter  { 0 };
    double              hostTicksPerFrame { 0.0 };
};
