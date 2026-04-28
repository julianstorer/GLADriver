
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
            // Remote routing: ch is both slot index and USB channel (1:1 mapping assumed)
            std::string name;
            for (auto const& r : avdecc.getEntities())
                if (r.id == eid) { name = r.name; break; }
            setSlot (static_cast<int> (ch), ch, eid, name);
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

    // Resize routing to n slots, all unassigned (channelIndex=0xFF = silence).
    // Call whenever the bridge or listener changes.
    void resetSlots (int n)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            routing.assign (static_cast<size_t> (std::max (0, n)),
                            RoutingEntry { 0xFF, 0, "" });
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (map);
    }

    // Assign USB input channel usbChannel to virtual output slot slot.
    void setSlot (int slot, uint8_t usbChannel, uint64_t entityId, const std::string& displayName)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            if (slot < 0 || slot >= static_cast<int> (routing.size())) return;
            routing[static_cast<size_t> (slot)] = { usbChannel, entityId, displayName };
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (map);
    }

    // Unassign virtual output slot slot (outputs silence; slot still exists in driver).
    void clearSlot (int slot)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            if (slot < 0 || slot >= static_cast<int> (routing.size())) return;
            routing[static_cast<size_t> (slot)] = { 0xFF, 0, "" };
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (map);
    }

    void setUSBBridge (const std::string& uid, const std::string& name = "")
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            usbBridgeUID  = uid;
            usbBridgeName = name;
        }
        ipc.broadcastUSBBridge (uid);
    }

    struct USBChannelInfo
    {
        uint8_t     channelIndex;
        uint64_t    talkerEntityId; // 0 = no AVDECC source
        std::string sourceName;     // empty if no source
    };

    // Returns one entry per USB input channel (0-based, count = totalChannels),
    // annotated with the AVDECC talker name feeding each channel (if any).
    // bridgeEntityId must be provided by the caller (selected by the user in the UI).
    std::vector<USBChannelInfo> getUSBChannelInfos (int totalChannels, uint64_t bridgeEntityId) const
    {
        std::vector<USBChannelInfo> result;
        if (totalChannels <= 0) return result;

        syslog (LOG_INFO, "GLA: getUSBChannelInfos: CoreAudio channels=%d entityId=0x%llx",
                totalChannels, (unsigned long long) bridgeEntityId);

        if (bridgeEntityId == 0)
        {
            // No AVDECC entity selected — show CoreAudio channels with no source info
            result.resize (static_cast<size_t> (totalChannels));
            for (int i = 0; i < totalChannels; ++i)
            {
                result[static_cast<size_t> (i)].channelIndex   = static_cast<uint8_t> (i);
                result[static_cast<size_t> (i)].talkerEntityId = 0;
                result[static_cast<size_t> (i)].sourceName     = "";
            }
            syslog (LOG_INFO, "GLA:   no AVDECC entity, showing %d CoreAudio channels", totalChannels);
            return result;
        }

        auto streamConns = avdecc.getListenerConnections (bridgeEntityId);
        int streamCount  = static_cast<int> (streamConns.size());
        syslog (LOG_INFO, "GLA:   AVDECC stream inputs=%d", streamCount);

        if (streamCount == 0)
        {
            // Entity found but no stream inputs — fall back to CoreAudio count
            result.resize (static_cast<size_t> (totalChannels));
            for (int i = 0; i < totalChannels; ++i)
            {
                result[static_cast<size_t> (i)].channelIndex   = static_cast<uint8_t> (i);
                result[static_cast<size_t> (i)].talkerEntityId = 0;
                result[static_cast<size_t> (i)].sourceName     = "";
            }
            syslog (LOG_WARNING, "GLA:   entity has no stream inputs, falling back to CoreAudio channels=%d", totalChannels);
            return result;
        }

        auto entities = avdecc.getEntities();
        int channelIndex = 0;

        for (auto const& sc : streamConns)
        {
            std::string sourceName;
            for (auto const& e : entities)
                if (e.id == sc.talkerEntityId) { sourceName = e.name; break; }

            syslog (LOG_INFO, "GLA:   stream[%d] talker=0x%llx channels=%d source='%s'",
                    sc.streamIndex, (unsigned long long) sc.talkerEntityId, sc.channelCount, sourceName.c_str());

            for (int ch = 0; ch < sc.channelCount; ++ch)
            {
                USBChannelInfo info;
                info.channelIndex   = static_cast<uint8_t> (channelIndex++);
                info.talkerEntityId = sc.talkerEntityId;
                info.sourceName     = sourceName;
                result.push_back (info);
            }
        }

        syslog (LOG_INFO, "GLA:   total AVDECC-derived channels=%d", channelIndex);
        return result;
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
    std::string usbBridgeName;

    AVDECCController avdecc;
    GLAIPCServer ipc;

    EntityListCallback onEntityList;
    ChannelMapCallback onChannelMap;
};
