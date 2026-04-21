#pragma once
#include "avdecc_controller.hpp"
#include "usb_bridge_monitor.hpp"
#include "gla_ipc_server.hpp"
#include "../common/gla_ipc_types.hpp"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

//==============================================================================
// Hosts the IPC server, AVDECC controller, and USB bridge monitor in-process.
// The HAL driver connects to the same Unix socket; the app owns the server end.
class AppBackend
{
public:
    using EntityListCallback = std::function<void (const std::vector<GLAEntityInfo>&)>;
    using ChannelMapCallback = std::function<void (const std::vector<GLAChannelEntry>&)>;

    AppBackend();
    ~AppBackend();

    //==============================================================================
    // Start IPC server and AVDECC discovery on the given network interface.
    // Returns false if the IPC server fails to bind (fatal); AVDECC failure is
    // non-fatal and logged.
    bool start (const std::string& networkInterface);
    void stop();

    void setNetworkInterface (const std::string& iface);
    void setRouting (uint8_t channelIndex, uint64_t entityId);
    void setUSBBridge (const std::string& uid);

    // Callbacks are delivered on the JUCE message thread.
    void setEntityListCallback (EntityListCallback cb) { onEntityList = std::move (cb); }
    void setChannelMapCallback (ChannelMapCallback cb) { onChannelMap = std::move (cb); }

private:
    //==============================================================================
    struct RoutingEntry
    {
        uint8_t     channelIndex;
        uint64_t    entityId;
        std::string displayName;
    };

    std::vector<GLAChannelEntry> buildChannelMap() const;
    std::vector<GLAEntityInfo>   buildEntityList() const;

    mutable std::mutex       mutex;
    std::vector<RoutingEntry> routing;
    std::string              usbBridgeUID;

    AVDECCController avdecc;
    USBBridgeMonitor bridgeMon;
    GLAIPCServer     ipc;

    EntityListCallback onEntityList;
    ChannelMapCallback onChannelMap;
};
