#pragma once

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <syslog.h>
#include <thread>
#include <vector>

// Reads audio from a CoreAudio device in the app process and calls back with
// interleaved float32 frames. Must run in a normal user process — the HAL
// plugin helper process receives zero-filled buffers due to sandbox restrictions.
struct GLAUSBCapture
{
    using AudioCallback = std::function<void (uint32_t channelCount, uint32_t frameCount,
                                               double sourceRate, const float* interleaved)>;

    GLAUSBCapture()  = default;
    ~GLAUSBCapture() { stop(); }

    bool start (const std::string& uid, AudioCallback cb)
    {
        stop();
        deviceId_ = findDeviceByUID (uid);

        if (deviceId_ == kAudioDeviceUnknown)
        {
            syslog (LOG_WARNING, "GLA: USBCapture: device '%s' not found", uid.c_str());
            return false;
        }

        AudioObjectPropertyAddress rateProp { kAudioDevicePropertyNominalSampleRate,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
        Float64 rate = 48000.0;
        UInt32  rateSz = sizeof (rate);

        if (AudioObjectGetPropertyData (deviceId_, &rateProp, 0, nullptr, &rateSz, &rate) != noErr
                || rate <= 0.0)
            rate = 48000.0;

        sourceRate_ = rate;
        callback_   = std::move (cb);

        // Pre-allocate so IOProc never triggers a heap allocation.
        pendingData_.reserve (32 * 512);

        senderStop_    = false;
        senderHasData_ = false;
        senderThread_  = std::thread ([this] { senderLoop(); });

        OSStatus err = AudioDeviceCreateIOProcID (deviceId_, ioProcStatic, this, &ioProcId_);

        if (err != noErr)
        {
            syslog (LOG_ERR, "GLA: USBCapture: AudioDeviceCreateIOProcID failed: %d", (int) err);
            stopSender();
            callback_ = {};
            return false;
        }

        err = AudioDeviceStart (deviceId_, ioProcId_);

        if (err != noErr)
        {
            AudioDeviceDestroyIOProcID (deviceId_, ioProcId_);
            ioProcId_ = nullptr;
            stopSender();
            callback_ = {};
            syslog (LOG_ERR, "GLA: USBCapture: AudioDeviceStart failed: %d", (int) err);
            return false;
        }

        running_ = true;
        syslog (LOG_INFO, "GLA: USBCapture started (deviceId %u, rate %.0f Hz)",
                deviceId_, sourceRate_);
        return true;
    }

    void stop()
    {
        if (! running_) return;
        AudioDeviceStop (deviceId_, ioProcId_);
        AudioDeviceDestroyIOProcID (deviceId_, ioProcId_);
        ioProcId_     = nullptr;
        running_      = false;
        layoutLogged_ = false;
        stopSender();
        callback_ = {};
        syslog (LOG_INFO, "GLA: USBCapture stopped");
    }

    bool isRunning() const { return running_; }

private:
    //==============================================================================
    void stopSender()
    {
        {
            std::lock_guard<std::mutex> lk (senderMutex_);
            senderStop_ = true;
        }
        senderCv_.notify_one();

        if (senderThread_.joinable())
            senderThread_.join();
    }

    // Dedicated sender thread — does the blocking socket write off the IO thread.
    void senderLoop()
    {
        std::vector<float> localData;
        uint32_t localChannels = 0, localFrames = 0;
        double   localRate     = 48000.0;

        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk (senderMutex_);
                senderCv_.wait (lk, [this] { return senderHasData_ || senderStop_; });

                if (senderStop_ && ! senderHasData_)
                    break;

                // Copy inside the lock — pendingData_ retains its capacity for
                // the next IOProc call so no heap allocation happens there.
                localData      = pendingData_;
                localChannels  = pendingChannels_;
                localFrames    = pendingFrames_;
                localRate      = pendingRate_;
                senderHasData_ = false;
            }

            if (callback_)
                callback_ (localChannels, localFrames, localRate, localData.data());
        }
    }

    static AudioDeviceID findDeviceByUID (const std::string& uid)
    {
        AudioObjectPropertyAddress prop { kAudioHardwarePropertyDevices,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        UInt32 dataSize = 0;
        AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);

        UInt32 count = dataSize / sizeof (AudioDeviceID);
        std::vector<AudioDeviceID> ids (count);
        AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, ids.data());

        for (auto id : ids)
        {
            AudioObjectPropertyAddress uidProp { kAudioDevicePropertyDeviceUID,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain };
            CFStringRef cfUID = nullptr;
            UInt32 sz = sizeof (cfUID);

            if (AudioObjectGetPropertyData (id, &uidProp, 0, nullptr, &sz, &cfUID) != noErr)
                continue;

            char buf[256] = {};
            CFStringGetCString (cfUID, buf, sizeof (buf), kCFStringEncodingUTF8);
            CFRelease (cfUID);

            if (uid == buf)
                return id;
        }

        return kAudioDeviceUnknown;
    }

    static OSStatus ioProcStatic (AudioDeviceID,
                                   const AudioTimeStamp*,
                                   const AudioBufferList* inputData,
                                   const AudioTimeStamp*,
                                   AudioBufferList*,
                                   const AudioTimeStamp*,
                                   void* client)
    {
        return static_cast<GLAUSBCapture*> (client)->ioProc (inputData);
    }

    OSStatus ioProc (const AudioBufferList* inputData)
    {
        if (! callback_ || ! inputData || inputData->mNumberBuffers == 0)
            return noErr;

        // Log actual device buffer layout once per capture session.
        if (! layoutLogged_)
        {
            layoutLogged_ = true;
            syslog (LOG_INFO, "GLA: USBCapture layout: %u AudioBuffer(s)",
                    inputData->mNumberBuffers);
            for (UInt32 b = 0; b < inputData->mNumberBuffers; ++b)
                syslog (LOG_INFO, "GLA:   buf[%u]: %u ch  %u bytes",
                        b,
                        inputData->mBuffers[b].mNumberChannels,
                        inputData->mBuffers[b].mDataByteSize);
        }

        // Sum channels and derive frame count across all buffers.
        // Devices with multiple streams present one AudioBuffer per stream.
        uint32_t totalChannels = 0;
        uint32_t frameCount    = 0;

        for (UInt32 b = 0; b < inputData->mNumberBuffers; ++b)
        {
            const auto& buf = inputData->mBuffers[b];

            if (! buf.mData || buf.mDataByteSize == 0 || buf.mNumberChannels == 0)
                continue;

            totalChannels += buf.mNumberChannels;

            if (frameCount == 0)
                frameCount = buf.mDataByteSize / (sizeof (float) * buf.mNumberChannels);
        }

        if (totalChannels == 0 || frameCount == 0)
            return noErr;

        // try_lock: never block on the real-time IO thread.
        // If the sender is still busy with the previous frame, drop this one.
        std::unique_lock<std::mutex> lk (senderMutex_, std::try_to_lock);

        if (! lk.owns_lock() || senderHasData_)
            return noErr;

        // Build one flat interleaved array: append each buffer's channels in order.
        // Stream 0 occupies positions 0..n0-1, stream 1 at n0..n0+n1-1, etc.
        pendingData_.resize (totalChannels * frameCount);
        uint32_t chOffset = 0;

        for (UInt32 b = 0; b < inputData->mNumberBuffers; ++b)
        {
            const auto& buf = inputData->mBuffers[b];

            if (! buf.mData || buf.mDataByteSize == 0 || buf.mNumberChannels == 0)
                continue;

            const float*   src = static_cast<const float*> (buf.mData);
            const uint32_t nCh = buf.mNumberChannels;

            for (uint32_t f = 0; f < frameCount; ++f)
                for (uint32_t c = 0; c < nCh; ++c)
                    pendingData_[f * totalChannels + chOffset + c] = src[f * nCh + c];

            chOffset += nCh;
        }

        pendingChannels_ = totalChannels;
        pendingFrames_   = frameCount;
        pendingRate_     = sourceRate_;
        senderHasData_   = true;
        senderCv_.notify_one();

        return noErr;
    }

    //==============================================================================
    AudioDeviceID       deviceId_      = kAudioDeviceUnknown;
    AudioDeviceIOProcID ioProcId_      = nullptr;
    bool                running_       = false;
    bool                layoutLogged_  = false;
    double              sourceRate_    = 48000.0;
    AudioCallback       callback_;

    std::thread             senderThread_;
    std::mutex              senderMutex_;
    std::condition_variable senderCv_;
    std::vector<float>      pendingData_;
    uint32_t                pendingChannels_ = 0;
    uint32_t                pendingFrames_   = 0;
    double                  pendingRate_     = 48000.0;
    bool                    senderHasData_   = false;
    bool                    senderStop_      = false;
};
