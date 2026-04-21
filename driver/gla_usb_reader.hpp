#pragma once
#include <CoreAudio/CoreAudio.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include "../common/gla_ring_buffer.hpp"

// Registers an AudioDeviceIOProc against the USB bridge device.
// On each callback, demultiplexes channels into per-source ring buffers.
class GLAUSBReader {
public:
    GLAUSBReader();
    ~GLAUSBReader();

    bool start(const std::string& deviceNameSubstring);
    void stop();

    bool isRunning() const { return _running; }
    AudioDeviceID deviceID() const { return _deviceID; }

    // Register a channel→ring buffer mapping. channelIndex is 0-based.
    void setChannelBuffer(int channelIndex, GLARingBuffer* buf);
    void clearChannelBuffers();

private:
    static OSStatus ioProcStatic(AudioDeviceID device,
                                 const AudioTimeStamp* nowTime,
                                 const AudioBufferList* inputData,
                                 const AudioTimeStamp* inputTime,
                                 AudioBufferList* outputData,
                                 const AudioTimeStamp* outputTime,
                                 void* clientData);

    OSStatus ioProc(const AudioBufferList* inputData);

    AudioDeviceID _deviceID   = kAudioDeviceUnknown;
    AudioDeviceIOProcID _ioProcID = nullptr;
    bool _running = false;

    std::mutex _mapMutex;
    std::vector<GLARingBuffer*> _channelBuffers; // index = USB channel (0-based), non-owning
    float _scratch[4096]; // deinterleave buffer; member avoids RT-thread stack pressure
};
