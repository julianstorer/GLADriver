#pragma once

#include <aspl/Device.hpp>
#include <aspl/IORequestHandler.hpp>
#include <aspl/Stream.hpp>
#include <atomic>
#include <dispatch/dispatch.h>
#include <functional>
#include <memory>
#include <vector>
#include <mach/mach_time.h>
#include <syslog.h>
#include "GLA_Log.h"
#include "../common/GLA_IPCTypes.h"
#include "../common/GLA_ResamplingFIFO.h"


// Single CoreAudio device. All AVB sources appear as channels in one interleaved stream.
struct GLAUnifiedDevice  : public aspl::Device
{
    GLAUnifiedDevice (std::shared_ptr<const aspl::Context> context,
                      const std::vector<GLAChannelEntry>& channelEntries)
        : aspl::Device (context, makeParams_ (channelEntries.size())),
          entries (channelEntries),
          rings (std::make_shared<FifoVec>())
    {
        // Compute timing here (not in init()) so GetZeroTimeStampImpl is valid
        // from construction, even for stub devices that never call init().
        struct mach_timebase_info tb;
        mach_timebase_info (&tb);
        hostTicksPerFrame = (double (tb.denom) / double (tb.numer)) * 1e9 / sampleRate_;

        for (size_t i = 0; i < channelEntries.size(); ++i)
            rings->push_back (std::make_unique<GLAResamplingFIFO> (double (sampleRate_)));
    }

    ~GLAUnifiedDevice() override
    {
        glaLog (LOG_INFO, "GLA: destroyed unified device (%zu channels)", entries.size());
    }

    // Called once before AddDevice, while HasOwner() == false, so all changes
    // apply in-place immediately without going through RequestConfigurationChange.
    void init()
    {
        const UInt32 nChannels = static_cast<UInt32> (entries.size());
        stream_ = AddStreamAsync (makeStreamParams_ (nChannels));

        // Pass shared ownership of the FIFO vector so the handler keeps the
        // buffers alive even if the device is removed while an IO cycle is
        // still in flight on coreaudiod's HAL thread.
        SetIOHandler (std::make_shared<UnifiedIOHandler> (nChannels, rings));
    }

    // Reconfigure the device's channel map without removing/re-adding it.
    // Uses RequestConfigurationChange so coreaudiod performs the swap at a
    // clean IO boundary. onRingsReady is called on the HAL non-realtime thread
    // immediately after the new FIFOs and IO handler are installed; callers use
    // it to point the USB reader at the new buffers.
    //
    // Called from the IPC client thread. Safe: ASPL guarantees AddDevice /
    // RemoveDevice / RequestConfigurationChange are all thread-safe.
    void updateChannelMap (const std::vector<GLAChannelEntry>& newEntries,
                           std::function<void()> onRingsReady = {})
    {
        RequestConfigurationChange ([this,
                                     newEntries,
                                     onRingsReady = std::move (onRingsReady)]() mutable
        {
            // --- Inside PerformConfigurationChange ---
            // HAL non-realtime thread; IO is quiesced for the duration.

            if (stream_)
            {
                RemoveStreamAsync (stream_);
                stream_.reset();
            }

            entries = newEntries;

            auto newRings = std::make_shared<FifoVec>();
            const UInt32 nCh = static_cast<UInt32> (newEntries.size());

            for (size_t i = 0; i < newEntries.size(); ++i)
                newRings->push_back (std::make_unique<GLAResamplingFIFO> (double (sampleRate_)));

            rings = newRings;

            stream_ = AddStreamAsync (makeStreamParams_ (nCh));
            SetIOHandler (std::make_shared<UnifiedIOHandler> (nCh, rings));

            if (onRingsReady)
                onRingsReady();
        });
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

    // Update channel entry metadata (names, entityIds) without reconfiguring the
    // device. Only valid when newEntries.size() == entries.size(). Called from the
    // fast path in applyChannelMap when only annotations change, not channel count.
    void updateEntries (const std::vector<GLAChannelEntry>& newEntries)
    {
        if (newEntries.size() == entries.size())
            entries = newEntries;
    }

    //==============================================================================
    GLAResamplingFIFO* getChannelFIFO (uint32_t slotIndex)
    {
        if (slotIndex >= rings->size()) return nullptr;
        return (*rings)[slotIndex].get();
    }

    size_t getRingCount() const { return rings ? rings->size() : 0; }

    static constexpr UInt32 sampleRate_  = 48000;
    static constexpr UInt32 zeroTsPeriod = 512;

protected:
    //==============================================================================
    OSStatus GetZeroTimeStampImpl (UInt32 /*clientID*/,
                                   Float64* outSampleTime,
                                   UInt64*  outHostTime,
                                   UInt64*  outSeed) override
    {
        // Guard: hostTicksPerFrame must be positive. If it is zero or negative
        // (which must never happen given constructor initialisation, but we defend
        // against it here), ticksPerPeriod would be 0 and the counter-advance
        // condition would always be true, spinning coreaudiod at 100% CPU.
        const double tpf = hostTicksPerFrame;
        if (tpf <= 0.0)
        {
            *outSampleTime = 0.0;
            *outHostTime   = mach_absolute_time();
            *outSeed       = 1;
            return kAudioHardwareNoError;
        }

        UInt64 anchor = anchorHostTime.load (std::memory_order_relaxed);

        if (anchor == 0)
        {
            anchor = mach_absolute_time();
            anchorHostTime.store (anchor, std::memory_order_relaxed);
        }

        const auto period = static_cast<UInt64> (GetZeroTimeStampPeriod());
        const double ticksPerPeriod = tpf * static_cast<double> (period);

        // Belt-and-suspenders: period is a compile-time constant (512) so this
        // can't happen, but a zero here makes newCtr blow up to UINT64_MAX and
        // coreaudiod spin at 100% CPU.
        if (ticksPerPeriod <= 0.0)
        {
            *outSampleTime = 0.0;
            *outHostTime   = mach_absolute_time();
            *outSeed       = 1;
            return kAudioHardwareNoError;
        }

        const UInt64 now = mach_absolute_time();
        // Advance counter to the current period in one shot — incrementing by 1
        // per call causes coreaudiod to spin at 100% CPU if many periods have elapsed.
        const UInt64 elapsed = (now >= anchor) ? (now - anchor) : 0;
        const UInt64 newCtr  = static_cast<UInt64> (double (elapsed) / ticksPerPeriod);
        UInt64 ctr = periodCounter.load (std::memory_order_relaxed);

        if (newCtr > ctr)
            periodCounter.store (ctr = newCtr, std::memory_order_relaxed);

        *outSampleTime = double (ctr) * static_cast<double> (period);
        *outHostTime   = anchor + UInt64 (double (ctr) * ticksPerPeriod);
        *outSeed       = 1;
        return kAudioHardwareNoError;
    }

private:
    //==============================================================================
    using FifoVec = std::vector<std::unique_ptr<GLAResamplingFIFO>>;

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

    static aspl::StreamParameters makeStreamParams_ (UInt32 nChannels)
    {
        aspl::StreamParameters sp;
        sp.Direction                = aspl::Direction::Input;
        sp.StartingChannel          = 1;
        sp.Format                   = {};
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
        return sp;
    }

    //==============================================================================
    struct UnifiedIOHandler   : public aspl::IORequestHandler
    {
        UnifiedIOHandler (UInt32 n, std::shared_ptr<FifoVec> r)
            : numChannels (n), rings (std::move (r))
        {}

        void OnReadClientInput (const std::shared_ptr<aspl::Client>& /*client*/,
                                const std::shared_ptr<aspl::Stream>& /*stream*/,
                                Float64 /*zeroTimestamp*/,
                                Float64 /*timestamp*/,
                                void* bytes,
                                UInt32 bytesCount) override
        {
            ++callCount;

            if (numChannels == 0 || !rings)
            {
                std::memset (bytes, 0, bytesCount);
                return;
            }

            const UInt32 frames = bytesCount / (sizeof (float) * numChannels);

            if (frames > 4096)
            {
                std::memset (bytes, 0, bytesCount);
                return;
            }

            std::memset (bytes, 0, bytesCount);
            auto out = static_cast<float*> (bytes);

            for (UInt32 ch = 0; ch < numChannels; ++ch)
            {
                if (ch >= rings->size() || ! (*rings)[ch])
                    continue;

                (*rings)[ch]->read (scratch, frames);

                for (UInt32 f = 0; f < frames; ++f)
                    out[f * numChannels + ch] = scratch[f];
            }
        }

    private:
        UInt32                   numChannels;
        std::shared_ptr<FifoVec> rings;   // shared — outlives the device if an IO cycle is in flight
        float                    scratch[4096];
        uint64_t                 callCount = 0;
    };

    //==============================================================================
    std::vector<GLAChannelEntry> entries;
    std::shared_ptr<FifoVec>     rings;
    std::shared_ptr<aspl::Stream>   stream_;   // null for stubs

    std::atomic<UInt64> anchorHostTime { 0 };
    std::atomic<UInt64> periodCounter  { 0 };
    double hostTicksPerFrame = 0.0;
};
