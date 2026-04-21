#include "usb_bridge_monitor.hpp"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <syslog.h>

static int getInputChannelCount (AudioDeviceID id)
{
    AudioObjectPropertyAddress prop = {
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
        return "";
    char buf[256] = {};
    CFStringGetCString (cfStr, buf, sizeof (buf), kCFStringEncodingUTF8);
    CFRelease (cfStr);
    return buf;
}

static std::vector<AudioDeviceInfo> enumerateInputDevices()
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    UInt32 count = dataSize / sizeof (AudioDeviceID);
    std::vector<AudioDeviceID> ids (count);
    AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize,
                                ids.data());

    std::vector<AudioDeviceInfo> result;
    for (auto devID : ids)
    {
        int channels = getInputChannelCount (devID);
        if (channels < 1) continue;
        auto name = getDeviceStringProperty (devID, kAudioObjectPropertyName);
        auto uid  = getDeviceStringProperty (devID, kAudioDevicePropertyDeviceUID);
        result.push_back ({ devID, name, uid, channels });
    }
    return result;
}

USBBridgeMonitor::USBBridgeMonitor()
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectAddPropertyListener (kAudioObjectSystemObject, &prop,
                                    devicesChangedListener, this);
}

USBBridgeMonitor::~USBBridgeMonitor()
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &prop,
                                       devicesChangedListener, this);
}

std::vector<AudioDeviceInfo> USBBridgeMonitor::getInputDevices() const
{
    return enumerateInputDevices();
}

std::optional<AudioDeviceInfo> USBBridgeMonitor::findDeviceByUID (const std::string& uid) const
{
    for (auto const& dev : enumerateInputDevices())
        if (dev.uid == uid) return dev;
    return std::nullopt;
}

OSStatus USBBridgeMonitor::devicesChangedListener (AudioObjectID /*objectID*/,
                                                    UInt32 /*count*/,
                                                    const AudioObjectPropertyAddress* /*addrs*/,
                                                    void* clientData)
{
    auto* self = static_cast<USBBridgeMonitor*> (clientData);
    if (self->onChange) self->onChange();
    return noErr;
}
