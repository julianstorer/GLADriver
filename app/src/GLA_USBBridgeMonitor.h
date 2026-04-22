
#pragma once

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <syslog.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
struct AudioDeviceInfo
{
    AudioDeviceID id;
    std::string   name;
    std::string   uid;
    int           inputChannels;
};

//==============================================================================
// Enumerates CoreAudio devices and watches for USB bridge appearance/disappearance.
struct USBBridgeMonitor
{
    USBBridgeMonitor()
    {
        AudioObjectPropertyAddress prop =
        {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        AudioObjectAddPropertyListener (kAudioObjectSystemObject, &prop, devicesChangedListener, this);
    }

    ~USBBridgeMonitor()
    {
        AudioObjectPropertyAddress prop =
        {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &prop, devicesChangedListener, this);
    }

    //==============================================================================
    using OnDevicesChangedCallback = std::function<void()>;

    void setOnChangeCallback (OnDevicesChangedCallback cb)      { onChange = std::move (cb); }

    // Returns all multi-channel input devices (potential USB bridges).
    std::vector<AudioDeviceInfo> getInputDevices() const
    {
        return enumerateInputDevices();
    }

    // Returns info for the device matching the given UID, or nullopt.
    std::optional<AudioDeviceInfo> findDeviceByUID (const std::string& uid) const
    {
        for (auto const& dev : enumerateInputDevices())
            if (dev.uid == uid) return dev;
        return std::nullopt;
    }

private:
    //==============================================================================
    OnDevicesChangedCallback onChange;

    static OSStatus devicesChangedListener (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* clientData)
    {
        if (auto self = static_cast<USBBridgeMonitor*> (clientData))
            if (self->onChange)
                self->onChange();

        return noErr;
    }

    static int getInputChannelCount (AudioDeviceID id)
    {
        AudioObjectPropertyAddress prop =
        {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };

        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize (id, &prop, 0, nullptr, &dataSize) != noErr)
            return 0;

        std::vector<uint8_t> buf (dataSize);
        if (AudioObjectGetPropertyData (id, &prop, 0, nullptr, &dataSize, buf.data()) != noErr)
            return 0;

        auto* list = reinterpret_cast<AudioBufferList*> (buf.data());
        int total = 0;

        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            total += static_cast<int> (list->mBuffers[i].mNumberChannels);

        return total;
    }

    static std::string getDeviceStringProperty (AudioDeviceID id, AudioObjectPropertySelector sel)
    {
        AudioObjectPropertyAddress prop = { sel, kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain };
        CFStringRef cfStr = nullptr;
        UInt32 sz = sizeof (cfStr);

        if (AudioObjectGetPropertyData (id, &prop, 0, nullptr, &sz, &cfStr) != noErr || !cfStr)
            return {};

        char buf[256] = {};
        CFStringGetCString (cfStr, buf, sizeof (buf), kCFStringEncodingUTF8);
        CFRelease (cfStr);
        return buf;
    }

    static std::vector<AudioDeviceInfo> enumerateInputDevices()
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

        std::vector<AudioDeviceInfo> result;

        for (auto devID : ids)
        {
            int channels = getInputChannelCount (devID);
            if (channels < 1)
                continue;

            auto name = getDeviceStringProperty (devID, kAudioObjectPropertyName);
            auto uid  = getDeviceStringProperty (devID, kAudioDevicePropertyDeviceUID);
            result.push_back ({ devID, name, uid, channels });
        }

        return result;
    }
};
