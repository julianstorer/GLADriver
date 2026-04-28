
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <syslog.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <juce_events/juce_events.h>
#include "GLA_IPCServer.h"
#include "../../common/GLA_IPCTypes.h"
#include "GLA_AVDECCController.h"

static constexpr const char* kGLADriverUID = "com.greenlight.gla-injector.unified";


//==============================================================================
// Hosts the IPC server, AVDECC controller, and USB bridge monitor in-process.
// The HAL driver connects to the same Unix socket; the app owns the server end.
class AppBackend
{
public:
    using EntityListCallback = std::function<void (const std::vector<GLAEntityInfo>&)>;
    using ChannelMapCallback = std::function<void (const std::vector<GLAChannelEntry>&)>;

    AppBackend() = default;
    ~AppBackend() { stop(); }

    //==============================================================================
    // Start IPC server and AVDECC discovery on the given network interface.
    // Returns false if the IPC server fails to bind (fatal); AVDECC failure is
    // non-fatal and logged.
    bool start (const std::string& networkInterface)
    {
        ipc.setSetRoutingCallback ([this] (uint8_t ch, uint64_t eid)
        {
            std::string name = "Unknown";

            for (auto const& r : avdecc.getEntities())
            {
                if (r.id == eid)
                {
                    name = r.name;
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lk (mutex);
                bool found = false;

                for (auto& r : routing)
                {
                    if (r.channelIndex == ch)
                    {
                        r.entityId = eid; r.displayName = name;
                        found = true;
                        break;
                    }
                }

                if (! found)
                    routing.push_back ({ ch, eid, name });
            }

            ipc.broadcastChannelMap (buildChannelMap());
        });

        ipc.setSetNetifCallback ([this] (const std::string& iface)
        {
            avdecc.stop();
            avdecc.start (iface);
        });

        ipc.setSetBridgeCallback ([this] (const std::string& uid)
        {
            {
                std::lock_guard<std::mutex> lk (mutex);
                usbBridgeUID = uid;
            }
            ipc.broadcastUSBBridge (uid);
        });

        avdecc.setOnChangeCallback ([this]()
        {
            auto entityList = buildEntityList();
            auto map        = buildChannelMap();

            juce::MessageManager::callAsync ([this,
                                              entityList = std::move (entityList),
                                              map        = std::move (map)]() mutable
            {
                ipc.broadcastEntityList (entityList);
                ipc.broadcastChannelMap (map);

                if (onEntityList)
                    onEntityList (entityList);
            });
        });

        if (! ipc.start())
        {
            syslog (LOG_ERR, "GLA: failed to start IPC server");
            return false;
        }

        if (! avdecc.start (networkInterface))
            syslog (LOG_WARNING, "GLA: AVDECC start failed; continuing without AVB discovery");

        return true;
    }

    void stop()
    {
        avdecc.stop();
        ipc.stop();
    }

    void setNetworkInterface (const std::string& iface)
    {
        avdecc.stop();
        avdecc.start (iface);
    }

    void setRouting (uint8_t channelIndex, uint64_t entityId)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);

            if (entityId == 0)
            {
                routing.erase (std::remove_if (routing.begin(), routing.end(),
                                               [channelIndex] (const RoutingEntry& r) {
                                                   return r.channelIndex == channelIndex;
                                               }),
                               routing.end());
            }
            else
            {
                std::string name;
                for (auto const& r : avdecc.getEntities())
                {
                    if (r.id == entityId) { name = r.name; break; }
                }

                bool found = false;
                for (auto& r : routing)
                {
                    if (r.channelIndex == channelIndex)
                    {
                        r.entityId = entityId;
                        r.displayName = name;
                        found = true;
                        break;
                    }
                }

                if (! found)
                    routing.push_back ({ channelIndex, entityId, name });
            }
        }

        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (map);
    }

    void setUSBBridge (const std::string& uid)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            usbBridgeUID = uid;
        }
        ipc.broadcastUSBBridge (uid);
    }

    // Callbacks are delivered on the JUCE message thread.
    void setEntityListCallback (EntityListCallback cb)   { onEntityList = std::move (cb); }
    void setChannelMapCallback (ChannelMapCallback cb)   { onChannelMap = std::move (cb); }

    // Returns a short status string for the UI. Safe to call from the message thread.
    juce::String getDriverStatus()
    {
        int  clients   = ipc.getConnectedClientCount();
        bool installed = isDriverBundleInstalled();
        bool hasDevice = isGLADevicePresent();

        if (clients > 0)
            return "Driver: connected" + (hasDevice ? juce::String() : " (no device yet)");

        if (! installed)
            return "Driver: not installed";

        if (hasDevice)
            return "Driver: installed, not connected";

        return "Driver: installed, waiting for channel map";
    }

private:
    //==============================================================================
    struct RoutingEntry
    {
        uint8_t channelIndex;
        uint64_t entityId;
        std::string displayName;
    };

    static bool isDriverBundleInstalled()
    {
        return access ("/Library/Audio/Plug-Ins/HAL/GLAInjector.driver", F_OK) == 0;
    }

    static bool isGLADevicePresent()
    {
        AudioObjectPropertyAddress prop { kAudioHardwarePropertyDevices,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop,
                                            0, nullptr, &dataSize) != noErr)
            return false;

        std::vector<AudioDeviceID> ids (dataSize / sizeof (AudioDeviceID));
        AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop,
                                    0, nullptr, &dataSize, ids.data());

        for (auto devId : ids)
        {
            AudioObjectPropertyAddress uidProp { kAudioDevicePropertyDeviceUID,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
            CFStringRef cfStr = nullptr;
            UInt32 sz = sizeof (cfStr);
            if (AudioObjectGetPropertyData (devId, &uidProp, 0, nullptr, &sz, &cfStr) != noErr || ! cfStr)
                continue;

            char buf[256] = {};
            CFStringGetCString (cfStr, buf, sizeof (buf), kCFStringEncodingUTF8);
            CFRelease (cfStr);

            if (std::string (buf) == kGLADriverUID)
                return true;
        }

        return false;
    }

    std::vector<GLAChannelEntry> buildChannelMap() const
    {
        std::vector<GLAChannelEntry> entries;
        std::lock_guard<std::mutex> lk (mutex);

        for (auto const& r : routing)
        {
            GLAChannelEntry e{};
            e.channelIndex = r.channelIndex;
            e.entityId = r.entityId;
            strncpy (e.displayName, r.displayName.c_str(), sizeof (e.displayName) - 1);
            entries.push_back (e);
        }

        return entries;
    }

    std::vector<GLAEntityInfo> buildEntityList() const
    {
        auto records = avdecc.getEntities();
        std::vector<GLAEntityInfo> list;

        for (auto const& r : records)
        {
            GLAEntityInfo info {};
            info.entityId = r.id;
            info.online = r.online;
            info.streamCount = 0;
            strncpy (info.name, r.name.c_str(), sizeof (info.name) - 1);
            list.push_back (info);
        }

        return list;
    }

    mutable std::mutex mutex;
    std::vector<RoutingEntry> routing;
    std::string usbBridgeUID;

    AVDECCController avdecc;
    GLAIPCServer ipc;

    EntityListCallback onEntityList;
    ChannelMapCallback onChannelMap;
};
