
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
#include "GLA_USBCapture.h"
#include "../../common/GLA_IPCTypes.h"
#include "../../common/GLA_ChannelMatrix.h"
#include "GLA_AVDECCController.h"

static constexpr const char* kGLADriverUID = "com.greenlight.gla-injector.unified";


//==============================================================================
// Hosts the IPC server, AVDECC controller, and USB bridge monitor in-process.
// The HAL driver connects to the same Unix socket; the app owns the server end.
class AppBackend
{
public:
    struct SlotConfig
    {
        uint8_t     usbChannel  { 0xFF };
        uint64_t    entityId    { 0 };
        std::string displayName;
    };

    using EntityListCallback = std::function<void (const std::vector<GLAEntityInfo>&)>;
    using ChannelMapCallback = std::function<void (const std::vector<SlotConfig>&)>;

    AppBackend() = default;
    ~AppBackend() { stop(); }

    //==============================================================================
    // Start IPC server and AVDECC discovery on the given network interface.
    // USB audio capture is started separately when a bridge is selected.
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
            startCapture (uid);
        });

        avdecc.setOnChangeCallback ([this]()
        {
            auto entityList = buildEntityList();

            juce::MessageManager::callAsync ([this,
                                              entityList = std::move (entityList)]() mutable
            {
                ipc.broadcastEntityList (entityList);

                // Do NOT broadcastChannelMap here: onEntityList → rebuildPatchbay →
                // initializeSlots already sends the authoritative channel map once.
                // A second broadcast here would trigger a redundant stop/restart cycle
                // on the USB reader in the driver, racing the one from initializeSlots.
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
        usbCapture.stop();
        avdecc.stop();
        ipc.stop();
    }

    void setNetworkInterface (const std::string& iface)
    {
        avdecc.stop();
        avdecc.start (iface);
    }

    // Atomically set the full slot table and broadcast once.
    // Use this instead of resetSlots + N×setSlot to avoid N+1 driver reconfigurations.
    void initializeSlots (const std::vector<SlotConfig>& slots)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            routing.resize (slots.size());
            for (size_t i = 0; i < slots.size(); ++i)
                routing[i] = { slots[i].usbChannel, slots[i].entityId, slots[i].displayName };
            rebuildMatrix();
        }
        auto map = buildChannelMap();
        syslog (LOG_INFO, "GLA: initializeSlots: broadcasting %zu-slot channel map to %d client(s)",
                slots.size(), ipc.getConnectedClientCount());
        for (size_t i = 0; i < map.size(); ++i)
            syslog (LOG_INFO, "GLA:   slot[%zu] entityId=0x%llx name='%s'",
                    i, (unsigned long long) map[i].entityId,
                    map[i].displayName);
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (slots);
    }

    // Resize routing to n slots, all unassigned (channelIndex=0xFF = silence).
    // Call whenever the bridge or listener changes.
    void resetSlots (int n)
    {
        std::vector<SlotConfig> slots (static_cast<size_t> (std::max (0, n)));
        {
            std::lock_guard<std::mutex> lk (mutex);
            routing.assign (slots.size(), RoutingEntry { 0xFF, 0, "" });
            rebuildMatrix();
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (slots);
    }

    // Assign USB input channel usbChannel to virtual output slot slot.
    void setSlot (int slot, uint8_t usbChannel, uint64_t entityId, const std::string& displayName)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            if (slot < 0 || slot >= static_cast<int> (routing.size())) return;
            routing[static_cast<size_t> (slot)] = { usbChannel, entityId, displayName };
            rebuildMatrix();
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (buildSlotConfigs());
    }

    // Unassign virtual output slot slot (outputs silence; slot still exists in driver).
    void clearSlot (int slot)
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            if (slot < 0 || slot >= static_cast<int> (routing.size())) return;
            routing[static_cast<size_t> (slot)] = { 0xFF, 0, "" };
            rebuildMatrix();
        }
        auto map = buildChannelMap();
        ipc.broadcastChannelMap (map);
        if (onChannelMap) onChannelMap (buildSlotConfigs());
    }

    void setUSBBridge (const std::string& uid, const std::string& name = "")
    {
        {
            std::lock_guard<std::mutex> lk (mutex);
            usbBridgeUID  = uid;
            usbBridgeName = name;
        }
        syslog (LOG_INFO, "GLA: setUSBBridge: broadcasting uid='%s' to %d client(s)",
                uid.c_str(), ipc.getConnectedClientCount());
        ipc.broadcastUSBBridge (uid);
        startCapture (uid);
    }

    struct USBChannelInfo
    {
        uint8_t     channelIndex;
        uint64_t    talkerEntityId; // 0 = no AVDECC source
        std::string sourceName;     // empty if no source

        bool operator== (const USBChannelInfo& o) const noexcept
        {
            return channelIndex   == o.channelIndex
                && talkerEntityId == o.talkerEntityId
                && sourceName     == o.sourceName;
        }
    };

    // Returns one entry per USB input channel (0-based, count = totalChannels),
    // annotated with the AVDECC talker name feeding each channel (if any).
    // bridgeEntityId must be provided by the caller (selected by the user in the UI).
    std::vector<USBChannelInfo> getUSBChannelInfos (int totalChannels, uint64_t bridgeEntityId) const
    {
        std::vector<USBChannelInfo> result;
        if (totalChannels <= 0) return result;

        // Always produce exactly totalChannels entries indexed by CoreAudio channel
        // position. AVDECC is used only to annotate channels with source names —
        // it never controls how many slots we create.
        result.resize (static_cast<size_t> (totalChannels));
        for (int i = 0; i < totalChannels; ++i)
        {
            result[static_cast<size_t> (i)].channelIndex   = static_cast<uint8_t> (i);
            result[static_cast<size_t> (i)].talkerEntityId = 0;
            result[static_cast<size_t> (i)].sourceName     = "";
        }

        syslog (LOG_INFO, "GLA: getUSBChannelInfos: CoreAudio channels=%d entityId=0x%llx",
                totalChannels, (unsigned long long) bridgeEntityId);

        if (!bridgeEntityId) return result;

        auto streamConns = avdecc.getListenerConnections (bridgeEntityId);
        syslog (LOG_INFO, "GLA:   AVDECC stream inputs=%d", (int) streamConns.size());

        if (streamConns.empty()) return result;

        auto entities = avdecc.getEntities();

        // Streams are in ascending streamIndex order. Each stream occupies consecutive
        // CoreAudio channels starting at channelOffset. Annotate those positions with
        // the connected talker's name; stop if we run off the end of the CoreAudio range.
        int channelOffset = 0;
        for (auto const& sc : streamConns)
        {
            if (sc.talkerEntityId != 0)
            {
                std::string sourceName;
                for (auto const& e : entities)
                    if (e.id == sc.talkerEntityId) { sourceName = e.name; break; }

                syslog (LOG_INFO, "GLA:   stream[%d] talker=0x%llx ch=%d offset=%d source='%s'",
                        sc.streamIndex, (unsigned long long) sc.talkerEntityId,
                        sc.channelCount, channelOffset, sourceName.c_str());

                for (int ch = 0; ch < sc.channelCount; ++ch)
                {
                    int physCh = channelOffset + ch;
                    if (physCh >= totalChannels) break;
                    result[static_cast<size_t> (physCh)].talkerEntityId = sc.talkerEntityId;
                    result[static_cast<size_t> (physCh)].sourceName     = sourceName;
                }
            }
            channelOffset += sc.channelCount;
        }

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

    std::vector<SlotConfig> buildSlotConfigs() const
    {
        std::vector<SlotConfig> configs;
        std::lock_guard<std::mutex> lk (mutex);

        for (auto const& r : routing)
            configs.push_back ({ r.channelIndex, r.entityId, r.displayName });

        return configs;
    }

    std::vector<GLAChannelEntry> buildChannelMap() const
    {
        std::vector<GLAChannelEntry> entries;
        std::lock_guard<std::mutex> lk (mutex);

        for (auto const& r : routing)
        {
            GLAChannelEntry e{};
            e.entityId = r.entityId;
            strncpy (e.displayName, r.displayName.c_str(), sizeof (e.displayName) - 1);
            entries.push_back (e);
        }

        return entries;
    }

    // Must be called under mutex. Rebuilds the channel matrix from current routing.
    void rebuildMatrix()
    {
        matrix_.numDstChannels = static_cast<uint32_t> (routing.size());
        matrix_.routes.clear();

        for (uint32_t slot = 0; slot < routing.size(); ++slot)
        {
            const uint8_t src = routing[slot].channelIndex;

            if (src != 0xFF)
                matrix_.routes.push_back ({ src, static_cast<uint8_t> (slot) });
        }
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

    void startCapture (const std::string& uid)
    {
        usbCapture.stop();

        if (uid.empty())
            return;

        usbCapture.start (uid, [this] (uint32_t ch, uint32_t fr, double rate, const float* data)
        {
            GLAChannelMatrix mat;
            {
                std::lock_guard<std::mutex> lk (mutex);
                mat = matrix_;
            }

            mappedAudio_.resize (mat.numDstChannels * fr);
            mat.apply (data, ch, mappedAudio_.data(), fr);
            ipc.sendAudioData (mat.numDstChannels, fr, rate, mappedAudio_.data());
        });
    }

    //==============================================================================
    mutable std::mutex mutex;
    std::vector<RoutingEntry> routing;
    GLAChannelMatrix  matrix_;
    std::vector<float> mappedAudio_;
    std::string usbBridgeUID;
    std::string usbBridgeName;

    GLAUSBCapture    usbCapture;
    AVDECCController avdecc;
    GLAIPCServer     ipc;

    EntityListCallback onEntityList;
    ChannelMapCallback onChannelMap;
};
