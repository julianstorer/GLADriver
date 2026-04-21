#include "gla_usb_reader.hpp"
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

void GLAUSBReader::setChannelBuffer(int channelIndex, GLARingBuffer* buf) {
    std::lock_guard<std::mutex> lk(_mapMutex);
    if (channelIndex >= static_cast<int>(_channelBuffers.size()))
        _channelBuffers.resize(static_cast<size_t>(channelIndex + 1), nullptr);
    _channelBuffers[static_cast<size_t>(channelIndex)] = buf;
}

void GLAUSBReader::clearChannelBuffers() {
    std::lock_guard<std::mutex> lk(_mapMutex);
    _channelBuffers.clear();
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
    if (!_mapMutex.try_lock()) return noErr;
    auto buffers = _channelBuffers; // snapshot
    _mapMutex.unlock();

    int globalCh = 0;
    for (UInt32 buf = 0; buf < inputData->mNumberBuffers; ++buf) {
        const auto& abuf = inputData->mBuffers[buf];
        const int channelCount = static_cast<int>(abuf.mNumberChannels);
        const float* src = static_cast<const float*>(abuf.mData);
        const UInt32 frames = abuf.mDataByteSize / (sizeof(float) * abuf.mNumberChannels);

        for (int ch = 0; ch < channelCount; ++ch, ++globalCh) {
            if (globalCh >= static_cast<int>(buffers.size())) break;
            GLARingBuffer* ring = buffers[static_cast<size_t>(globalCh)];
            if (!ring) continue;

            if (channelCount == 1) {
                ring->write(src, frames);
            } else {
                for (UInt32 f = 0; f < frames; ++f)
                    _scratch[f] = src[f * static_cast<UInt32>(channelCount) + static_cast<UInt32>(ch)];
                ring->write(_scratch, frames);
            }
        }
    }
    return noErr;
}
