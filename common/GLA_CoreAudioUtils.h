
#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>

inline AudioDeviceID glaFindDeviceByUID (const std::string& uid)
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
