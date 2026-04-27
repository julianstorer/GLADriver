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
#include "../common/GLA_IPCTypes.h"
#include "../common/GLA_RingBuffer.h"


// Single CoreAudio device. All AVB sources appear as channels in one interleaved stream.
struct GLAUnifiedDevice  : public aspl::Device
{
    GLAUnifiedDevice (std::shared_ptr<const aspl::Context> context,
                      const std::vector<GLAChannelEntry>& channelEntries)
        : aspl::Device (context, makeParams_ (channelEntries.size())),
          entries (channelEntries)
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

        auto handler = std::make_shared<UnifiedIOHandler> (nChannels, &rings);
        SetIOHandler (handler);
    }

    //==============================================================================
    Boolean HasProperty (AudioObjectID objectID,
                         pid_t clientPID,
                         const AudioObjectPropertyAddress* address) const override
    {
        if (address
            && address->mSelector == kAudioObjectPropertyElementName
            && address->mElement >= 1
            && address->mElement <= static_cast<UInt32> (entries.size()))
            return true;
        return aspl::Device::HasProperty (objectID, clientPID, address);
    }

    OSStatus GetPropertyDataSize (AudioObjectID objectID,
                                  pid_t clientPID,
                                  const AudioObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize,
                                  const void* qualifierData,
                                  UInt32* outDataSize) const override
    {
        if (address
            && address->mSelector == kAudioObjectPropertyElementName
            && address->mElement >= 1
            && address->mElement <= static_cast<UInt32> (entries.size()))
        {
            *outDataSize = sizeof (CFStringRef);
            return kAudioHardwareNoError;
        }
        return aspl::Device::GetPropertyDataSize (
            objectID, clientPID, address, qualifierDataSize, qualifierData, outDataSize);
    }

    OSStatus GetPropertyData (AudioObjectID objectID,
                              pid_t clientPID,
                              const AudioObjectPropertyAddress* address,
                              UInt32 qualifierDataSize,
                              const void* qualifierData,
                              UInt32 inDataSize,
                              UInt32* outDataSize,
                              void* outData) const override
    {
        if (address
            && address->mSelector == kAudioObjectPropertyElementName
            && address->mElement >= 1
            && address->mElement <= static_cast<UInt32> (entries.size())
            && inDataSize >= sizeof (CFStringRef))
        {
            const auto& entry = entries[address->mElement - 1];
            CFStringRef name = CFStringCreateWithCString (
                kCFAllocatorDefault, entry.displayName, kCFStringEncodingUTF8);
            memcpy (outData, &name, sizeof (CFStringRef));
            *outDataSize = sizeof (CFStringRef);
            return kAudioHardwareNoError;
        }
        return aspl::Device::GetPropertyData (
            objectID, clientPID, address, qualifierDataSize, qualifierData,
            inDataSize, outDataSize, outData);
    }

    //==============================================================================
    GLARingBuffer* getChannelRingBuffer (int channelIndex)
    {
        auto it = channelToSlot.find (channelIndex);
        if (it == channelToSlot.end()) return nullptr;
        return rings[it->second].get();
    }

    std::vector<std::unique_ptr<GLARingBuffer>>& ringBuffers() { return rings; }

    static constexpr UInt32 sampleRate_   = 48000;
    static constexpr size_t ringCapacity_ = 4096;
    static constexpr UInt32 zeroTsPeriod  = 512;

protected:
    //==============================================================================
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
    //==============================================================================
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

    //==============================================================================
    struct UnifiedIOHandler   : public aspl::IORequestHandler
    {
        UnifiedIOHandler (UInt32 n, std::vector<std::unique_ptr<GLARingBuffer>>* r)
            : numChannels (n), rings (r)
        {}

        void OnReadClientInput (const std::shared_ptr<aspl::Client>& /*client*/,
                                const std::shared_ptr<aspl::Stream>& /*stream*/,
                                Float64 /*zeroTimestamp*/,
                                Float64 /*timestamp*/,
                                void* bytes,
                                UInt32 bytesCount) override
        {
            if (numChannels == 0 || !rings)
            {
                std::memset (bytes, 0, bytesCount);
                return;
            }

            const UInt32 frames = bytesCount / (sizeof (float) * numChannels);
            auto out = static_cast<float*> (bytes);

            for (UInt32 ch = 0; ch < numChannels; ++ch)
            {
                if (ch >= rings->size() || !(*rings)[ch])
                    continue;

                (*rings)[ch]->read (scratch, frames);

                for (UInt32 f = 0; f < frames; ++f)
                    out[f * numChannels + ch] = scratch[f];
            }
        }

    private:
        UInt32 numChannels;
        std::vector<std::unique_ptr<GLARingBuffer>>* rings;
        float scratch[4096];
    };

    //==============================================================================
    std::vector<GLAChannelEntry> entries;
    std::unordered_map<int, size_t> channelToSlot;
    std::vector<std::unique_ptr<GLARingBuffer>> rings;

    std::atomic<UInt64> anchorHostTime { 0 };
    std::atomic<UInt64> periodCounter  { 0 };
    double hostTicksPerFrame = 0;
};
