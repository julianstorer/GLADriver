#pragma once
#include <CoreAudio/CoreAudio.h>
#include <mutex>
#include <string>
#include <vector>
#include "../common/gla_ring_buffer.hpp"

// Registers an AudioDeviceIOProc against the USB bridge device.
// On each callback, demultiplexes channels into per-source ring buffers.
class GLAUSBReader
{
public:
    GLAUSBReader();
    ~GLAUSBReader();

    bool start (const std::string& deviceNameSubstring);
    void stop();

    bool          isRunning() const { return running; }
    AudioDeviceID getDeviceId() const { return deviceId; }

    // Register a channel->ring buffer mapping. channelIndex is 0-based.
    void setChannelBuffer (int channelIndex, GLARingBuffer* buf);
    void clearChannelBuffers();

private:
    static OSStatus ioProcStatic (AudioDeviceID device,
                                  const AudioTimeStamp* nowTime,
                                  const AudioBufferList* inputData,
                                  const AudioTimeStamp* inputTime,
                                  AudioBufferList* outputData,
                                  const AudioTimeStamp* outputTime,
                                  void* clientData);

    OSStatus ioProc (const AudioBufferList* inputData);

    AudioDeviceID     deviceId   = kAudioDeviceUnknown;
    AudioDeviceIOProcID ioProcId = nullptr;
    bool              running    = false;

    std::mutex               mapMutex;
    std::vector<GLARingBuffer*> channelBuffers; // index = USB channel (0-based), non-owning
    float                    scratch[4096];
};
