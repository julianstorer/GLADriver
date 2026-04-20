#pragma once
#include <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>
#include <functional>

struct AudioDeviceInfo {
    AudioDeviceID id;
    std::string name;
    std::string uid;
    int inputChannels;
};

// Enumerates CoreAudio devices and watches for USB bridge appearance/disappearance.
class USBBridgeMonitor {
public:
    using OnDevicesChangedCallback = std::function<void()>;

    USBBridgeMonitor();
    ~USBBridgeMonitor();

    void setOnChangeCallback(OnDevicesChangedCallback cb) { _onChange = std::move(cb); }

    // Returns all multi-channel input devices (potential USB bridges).
    std::vector<AudioDeviceInfo> getInputDevices() const;

    // Returns info for the device matching the given UID, or nullopt.
    std::optional<AudioDeviceInfo> findDeviceByUID(const std::string& uid) const;

private:
    static OSStatus devicesChangedListener(AudioObjectID objectID,
                                           UInt32 numberAddresses,
                                           const AudioObjectPropertyAddress* addresses,
                                           void* clientData);

    OnDevicesChangedCallback _onChange;
};
