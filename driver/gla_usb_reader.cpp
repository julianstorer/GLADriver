#include "gla_usb_reader.hpp"
#include "gla_device.hpp"
#include <CoreAudio/CoreAudio.h>
#include <syslog.h>
#include <cstring>

static AudioDeviceID findDeviceByName(const std::string& nameSubstr) {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    UInt32 count = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize,
                               ids.data());

    for (auto id : ids) {
        AudioObjectPropertyAddress nameProp = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef cfName = nullptr;
        UInt32 sz = sizeof(cfName);
        if (AudioObjectGetPropertyData(id, &nameProp, 0, nullptr, &sz, &cfName) != noErr)
            continue;
        char buf[256] = {};
        CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(cfName);
        if (std::string(buf).find(nameSubstr) != std::string::npos)
            return id;
    }
    return kAudioDeviceUnknown;
}

GLAUSBReader::GLAUSBReader() = default;

GLAUSBReader::~GLAUSBReader() {
    stop();
}

bool GLAUSBReader::start(const std::string& deviceNameSubstring) {
    stop();
    _deviceID = findDeviceByName(deviceNameSubstring);
    if (_deviceID == kAudioDeviceUnknown) {
        syslog(LOG_WARNING, "GLA: USB bridge device '%s' not found",
               deviceNameSubstring.c_str());
        return false;
    }

    // Request a small buffer to minimise latency.
    AudioObjectPropertyAddress bufProp = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    UInt32 frameSize = 128;
    AudioObjectSetPropertyData(_deviceID, &bufProp, 0, nullptr,
                               sizeof(frameSize), &frameSize);

    OSStatus err = AudioDeviceCreateIOProcID(_deviceID, ioProcStatic, this, &_ioProcID);
    if (err != noErr) {
        syslog(LOG_ERR, "GLA: AudioDeviceCreateIOProcID failed: %d", (int)err);
        return false;
    }
    err = AudioDeviceStart(_deviceID, _ioProcID);
    if (err != noErr) {
        AudioDeviceDestroyIOProcID(_deviceID, _ioProcID);
        _ioProcID = nullptr;
        syslog(LOG_ERR, "GLA: AudioDeviceStart failed: %d", (int)err);
        return false;
    }
    _running = true;
    syslog(LOG_INFO, "GLA: USB reader started (deviceID %u)", _deviceID);
    return true;
}

void GLAUSBReader::stop() {
    if (!_running) return;
    AudioDeviceStop(_deviceID, _ioProcID);
    AudioDeviceDestroyIOProcID(_deviceID, _ioProcID);
    _ioProcID = nullptr;
    _running = false;
}

void GLAUSBReader::setChannelDevice(int channelIndex, GLAEntityDevice* device) {
    std::lock_guard<std::mutex> lk(_mapMutex);
    if (channelIndex >= static_cast<int>(_channelDevices.size()))
        _channelDevices.resize(static_cast<size_t>(channelIndex + 1), nullptr);
    _channelDevices[static_cast<size_t>(channelIndex)] = device;
}

void GLAUSBReader::clearChannelDevices() {
    std::lock_guard<std::mutex> lk(_mapMutex);
    _channelDevices.clear();
}

OSStatus GLAUSBReader::ioProcStatic(AudioDeviceID /*device*/,
                                     const AudioTimeStamp* /*nowTime*/,
                                     const AudioBufferList* inputData,
                                     const AudioTimeStamp* /*inputTime*/,
                                     AudioBufferList* /*outputData*/,
                                     const AudioTimeStamp* /*outputTime*/,
                                     void* clientData)
{
    return static_cast<GLAUSBReader*>(clientData)->ioProc(inputData);
}

OSStatus GLAUSBReader::ioProc(const AudioBufferList* inputData) {
    // inputData is interleaved or non-interleaved float32 from the USB device.
    // Lock-free fast path: no mutex on audio thread.
    // We take a snapshot of the channel map under a try_lock; if we can't get
    // it we simply skip this callback (avoids priority inversion).
    if (!_mapMutex.try_lock()) return noErr;
    auto devices = _channelDevices; // snapshot
    _mapMutex.unlock();

    // USB bridge typically presents non-interleaved buffers (one per channel).
    for (UInt32 buf = 0; buf < inputData->mNumberBuffers; ++buf) {
        const auto& abuf = inputData->mBuffers[buf];
        int channelCount = static_cast<int>(abuf.mNumberChannels);
        const float* src = static_cast<const float*>(abuf.mData);
        UInt32 frames = abuf.mDataByteSize / (sizeof(float) * abuf.mNumberChannels);

        for (int ch = 0; ch < channelCount; ++ch) {
            int globalCh = static_cast<int>(buf) + ch; // adjust for interleaved
            if (globalCh >= static_cast<int>(devices.size())) continue;
            GLAEntityDevice* dev = devices[static_cast<size_t>(globalCh)];
            if (!dev) continue;

            if (channelCount == 1) {
                // Non-interleaved: entire buffer is this channel.
                dev->ringBuffer().write(src, frames);
            } else {
                // Interleaved: stride copy.
                for (UInt32 f = 0; f < frames; ++f)
                    dev->ringBuffer().write(src + f * channelCount + ch, 1);
            }
        }
    }
    return noErr;
}
