#pragma once

#include <CoreAudio/CoreAudio.h>
#include <cstring>
#include <mutex>
#include <string>
#include <syslog.h>
#include <vector>
#include "GLA_Log.h"
#include "../common/GLA_ResamplingFIFO.h"

struct GLAUSBReader
{
    GLAUSBReader() = default;
    ~GLAUSBReader() { stop(); }

    bool start (const std::string& uid)
    {
        stop();
        deviceId = findDeviceByUID (uid);

        if (deviceId == kAudioDeviceUnknown)
        {
            glaLog (LOG_WARNING, "GLA: USB bridge device '%s' not found", uid.c_str());
            return false;
        }

        scratch.resize (4096);

        // Query the USB device's nominal sample rate so the resampling FIFOs can
        // be configured before the first callback arrives.
        AudioObjectPropertyAddress rateProp { kAudioDevicePropertyNominalSampleRate,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
        Float64 srcRate = 48000.0;
        UInt32  rateSz  = sizeof (srcRate);
        if (AudioObjectGetPropertyData (deviceId, &rateProp, 0, nullptr, &rateSz, &srcRate) != noErr
                || srcRate <= 0.0)
            srcRate = 48000.0;

        glaLog (LOG_INFO, "GLA: USB device sample rate %.0f Hz", srcRate);

        // Log the virtual stream format (what the IOProc actually receives).
        AudioObjectPropertyAddress streamsProp { kAudioDevicePropertyStreams,
                                                  kAudioDevicePropertyScopeInput,
                                                  kAudioObjectPropertyElementMain };
        UInt32 streamsSz = 0;
        if (AudioObjectGetPropertyDataSize (deviceId, &streamsProp, 0, nullptr, &streamsSz) == noErr
                && streamsSz > 0)
        {
            std::vector<AudioStreamID> streamIds (streamsSz / sizeof (AudioStreamID));
            AudioObjectGetPropertyData (deviceId, &streamsProp, 0, nullptr, &streamsSz, streamIds.data());

            for (size_t si = 0; si < streamIds.size(); ++si)
            {
                AudioStreamBasicDescription fmt {};
                UInt32 fmtSz = sizeof (fmt);
                AudioObjectPropertyAddress fmtProp { kAudioStreamPropertyVirtualFormat,
                                                      kAudioObjectPropertyScopeGlobal,
                                                      kAudioObjectPropertyElementMain };
                AudioObjectGetPropertyData (streamIds[si], &fmtProp, 0, nullptr, &fmtSz, &fmt);
                glaLog (LOG_INFO,
                        "GLA: USB stream[%zu] virtualFmt: rate=%.0f fmt=%u flags=0x%x "
                        "ch=%u bitsPerCh=%u bytesPerFrame=%u",
                        si, fmt.mSampleRate, (unsigned) fmt.mFormatID, (unsigned) fmt.mFormatFlags,
                        (unsigned) fmt.mChannelsPerFrame, (unsigned) fmt.mBitsPerChannel,
                        (unsigned) fmt.mBytesPerFrame);
            }
        }

        {
            std::lock_guard<std::mutex> lk (mapMutex);
            for (auto* fifo : channelBuffers)
                if (fifo) fifo->setSourceRate (static_cast<double> (srcRate));
        }

        OSStatus err = AudioDeviceCreateIOProcID (deviceId, ioProcStatic, this, &ioProcId);

        if (err != noErr)
        {
            glaLog (LOG_ERR, "GLA: AudioDeviceCreateIOProcID failed: %d", (int) err);
            return false;
        }

        err = AudioDeviceStart (deviceId, ioProcId);

        if (err != noErr)
        {
            AudioDeviceDestroyIOProcID (deviceId, ioProcId);
            ioProcId = nullptr;
            glaLog (LOG_ERR, "GLA: AudioDeviceStart failed: %d", (int) err);
            return false;
        }

        running = true;
        glaLog (LOG_INFO, "GLA: USB reader started (deviceId %u)", deviceId);
        return true;
    }

    void stop()
    {
        if (!running) return;
        AudioDeviceStop (deviceId, ioProcId);
        AudioDeviceDestroyIOProcID (deviceId, ioProcId);
        ioProcId = nullptr;
        running  = false;
    }

    bool isRunning() const              { return running; }
    AudioDeviceID getDeviceId() const   { return deviceId; }

    void setChannelBuffer (int channelIndex, GLAResamplingFIFO* buf)
    {
        std::lock_guard<std::mutex> lk (mapMutex);
        if (channelIndex >= static_cast<int> (channelBuffers.size()))
            channelBuffers.resize (static_cast<size_t> (channelIndex + 1), nullptr);
        channelBuffers[static_cast<size_t> (channelIndex)] = buf;
    }

    void clearChannelBuffers()
    {
        std::lock_guard<std::mutex> lk (mapMutex);
        channelBuffers.clear();
    }

private:
    static AudioDeviceID findDeviceByUID (const std::string& uid)
    {
        AudioObjectPropertyAddress prop =
        {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        UInt32 dataSize = 0;
        AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);

        UInt32 count = dataSize / sizeof (AudioDeviceID);
        std::vector<AudioDeviceID> ids (count);
        AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, ids.data());

        for (auto id : ids)
        {
            AudioObjectPropertyAddress uidProp =
            {
                kAudioDevicePropertyDeviceUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };

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

    static OSStatus ioProcStatic (AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* inputData,
                                  const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* clientData)
    {
        return static_cast<GLAUSBReader*> (clientData)->ioProc (inputData);
    }

    OSStatus ioProc (const AudioBufferList* inputData)
    {
        if (! mapMutex.try_lock())
            return noErr;

        auto buffers = channelBuffers;
        mapMutex.unlock();

        ++ioProcCallCount;

        int globalCh = 0;

        for (UInt32 b = 0; b < inputData->mNumberBuffers; ++b)
        {
            const auto& abuf = inputData->mBuffers[b];
            const int channelCount = static_cast<int> (abuf.mNumberChannels);
            const float* src = static_cast<const float*> (abuf.mData);
            const UInt32 frames = abuf.mDataByteSize / (sizeof (float) * abuf.mNumberChannels);

            for (int ch = 0; ch < channelCount; ++ch, ++globalCh)
            {
                if (globalCh >= static_cast<int> (buffers.size()))
                    break;

                auto fifo = buffers[static_cast<size_t> (globalCh)];

                if (!fifo)
                    continue;

                if (channelCount == 1)
                {
                    fifo->write (src, frames);
                }
                else
                {
                    for (UInt32 f = 0; f < frames; ++f)
                        scratch[f] = src[f * static_cast<UInt32> (channelCount) + static_cast<UInt32> (ch)];

                    fifo->write (scratch.data(), frames);
                }
            }
        }

        return noErr;
    }

    AudioDeviceID deviceId = kAudioDeviceUnknown;
    AudioDeviceIOProcID ioProcId = nullptr;
    bool running = false;

    std::mutex mapMutex;
    std::vector<GLAResamplingFIFO*> channelBuffers;
    std::vector<float> scratch;
    uint64_t ioProcCallCount = 0;
};
