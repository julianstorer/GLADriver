#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "../common/gla_ipc_types.hpp"

//==============================================================================
// Single-threaded UNIX socket server that serves:
//   - The HAL driver (receives ChannelMapUpdate)
//   - The companion app (bidirectional config commands)
class GLAIPCServer
{
public:
    using SetRoutingCallback = std::function<void (uint8_t channelIndex, uint64_t entityId)>;
    using SetNetifCallback   = std::function<void (const std::string& iface)>;
    using SetBridgeCallback  = std::function<void (const std::string& uid)>;

    GLAIPCServer();
    ~GLAIPCServer();

    //==============================================================================
    void setSetRoutingCallback (SetRoutingCallback cb) { onSetRouting = std::move (cb); }
    void setSetNetifCallback   (SetNetifCallback   cb) { onSetNetif   = std::move (cb); }
    void setSetBridgeCallback  (SetBridgeCallback  cb) { onSetBridge  = std::move (cb); }

    bool start();
    void stop();

    // Push channel map update to all connected clients.
    void broadcastChannelMap (const std::vector<GLAChannelEntry>& entries);

    // Push entity list to all connected app clients.
    void broadcastEntityList (const std::vector<GLAEntityInfo>& entities);

private:
    //==============================================================================
    void runLoop();
    void handleClientMessage (int clientFd, const std::vector<uint8_t>& msg);
    void removeDeadClients();

    int               serverFd = -1;
    std::atomic<bool> running  { false };
    std::thread       thread;

    std::mutex        clientsMutex;
    std::vector<int>  clients; // connected client fds

    SetRoutingCallback onSetRouting;
    SetNetifCallback   onSetNetif;
    SetBridgeCallback  onSetBridge;
};
