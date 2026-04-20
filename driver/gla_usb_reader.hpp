#pragma once
#include <CoreAudio/CoreAudio.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

class GLAEntityDevice;

// Registers an AudioDeviceIOProc against the USB bridge device.
// On each callback, demultiplexes channels into per-entity ring buffers.
class GLAUSBReader {
public:
    GLAUSBReader();
    ~GLAUSBReader();

    // Find the USB bridge device by matching substring of its name.
    // Returns true if found and started.
    bool start(const std::string& deviceNameSubstring);
    void stop();

    bool isRunning() const { return _running; }
    AudioDeviceID deviceID() const { return _deviceID; }

    // Register a channel→device mapping. channelIndex is 0-based.
    void setChannelDevice(int channelIndex, GLAEntityDevice* device);
    void clearChannelDevices();

private:
    static OSStatus ioProcStatic(AudioDeviceID device,
                                 const AudioTimeStamp* nowTime,
                                 const AudioBufferList* inputData,
                                 const AudioTimeStamp* inputTime,
                                 AudioBufferList* outputData,
                                 const AudioTimeStamp* outputTime,
                                 void* clientData);

    OSStatus ioProc(const AudioBufferList* inputData);

    AudioDeviceID _deviceID = kAudioDeviceUnknown;
    AudioDeviceIOProcID _ioProcID = nullptr;
    bool _running = false;

    std::mutex _mapMutex;
    std::vector<GLAEntityDevice*> _channelDevices; // index = USB channel (0-based)
};
