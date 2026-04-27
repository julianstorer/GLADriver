
#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <poll.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../../common/GLA_IPCTypes.h"
#include "../../common/GLA_Socket.h"

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

    GLAIPCServer() = default;
    ~GLAIPCServer() { stop(); }

    //==============================================================================
    void setSetRoutingCallback (SetRoutingCallback cb)    { onSetRouting = std::move (cb); }
    void setSetNetifCallback   (SetNetifCallback   cb)    { onSetNetif   = std::move (cb); }
    void setSetBridgeCallback  (SetBridgeCallback  cb)    { onSetBridge  = std::move (cb); }

    bool start()
    {
        serverFd = glaCreateServer (glaSocketPath);

        if (serverFd < 0)
        {
            syslog (LOG_ERR, "GLA: failed to create IPC server socket");
            return false;
        }

        running = true;
        thread  = std::thread ([this] { runLoop(); });
        syslog (LOG_INFO, "GLA: IPC server started at %s", glaSocketPath);
        return true;
    }

    void stop()
    {
        running = false;

        if (serverFd >= 0)
        {
            close (serverFd);
            serverFd = -1;
        }

        if (thread.joinable())
            thread.join();

        std::lock_guard<std::mutex> lk (clientsMutex);

        for (int fd : clients)
            close (fd);

        clients.clear();
    }

    void broadcastChannelMap (const std::vector<GLAChannelEntry>& entries)
    {
        auto payload = serializeChannelMapUpdate (entries);
        std::lock_guard<std::mutex> lk (clientsMutex);
        currentChannelMapMsg = payload;
        std::vector<int> dead;

        for (int fd : clients)
            if (! glaSendMessage (fd, payload))
                dead.push_back (fd);

        for (int fd : dead)
        {
            close (fd);
            clients.erase (std::remove (clients.begin(), clients.end(), fd), clients.end());
        }
    }

    void broadcastEntityList (const std::vector<GLAEntityInfo>& entities)
    {
        auto payload = serializeEntityList (entities);
        std::lock_guard<std::mutex> lk (clientsMutex);
        std::vector<int> dead;

        for (int fd : clients)
            if (! glaSendMessage (fd, payload))
                dead.push_back (fd);

        for (int fd : dead)
        {
            close (fd);
            clients.erase (std::remove (clients.begin(), clients.end(), fd), clients.end());
        }
    }

    void broadcastUSBBridge (const std::string& uid)
    {
        auto payload = serializeUSBBridge (uid);
        std::lock_guard<std::mutex> lk (clientsMutex);
        currentBridgeUID = uid;
        std::vector<int> dead;

        for (int fd : clients)
            if (! glaSendMessage (fd, payload))
                dead.push_back (fd);

        for (int fd : dead)
        {
            close (fd);
            clients.erase (std::remove (clients.begin(), clients.end(), fd), clients.end());
        }
    }

    int getConnectedClientCount()
    {
        std::lock_guard<std::mutex> lk (clientsMutex);
        return static_cast<int> (clients.size());
    }

private:
    //==============================================================================
    void runLoop()
    {
        while (running)
        {
            std::vector<pollfd> pfds;

            {
                std::lock_guard<std::mutex> lk (clientsMutex);
                pfds.reserve (1 + clients.size());
                pfds.push_back ({ serverFd, POLLIN, 0 });

                for (int fd : clients)
                    pfds.push_back ({ fd, POLLIN, 0 });
            }

            // 1 s timeout so we can recheck `running` even when idle.
            int ret = poll (pfds.data(), static_cast<nfds_t> (pfds.size()), 1000);

            if (ret <= 0)
                continue;

            if (pfds[0].revents & POLLIN)
            {
                if (int clientFd = glaAcceptClient (serverFd); clientFd >= 0)
                {
                    std::lock_guard<std::mutex> lk (clientsMutex);
                    clients.push_back (clientFd);
                    syslog (LOG_INFO, "GLA: client connected (fd %d)", clientFd);

                    if (! currentBridgeUID.empty())
                        glaSendMessage (clientFd, serializeUSBBridge (currentBridgeUID));

                    if (! currentChannelMapMsg.empty())
                        glaSendMessage (clientFd, currentChannelMapMsg);
                }
            }

            // Snapshot client list; indices in pfds[1..] match clients at snapshot time.
            std::vector<int> clientsCopy;

            {
                std::lock_guard<std::mutex> lk (clientsMutex);
                clientsCopy = clients;
            }

            for (size_t i = 0; i < clientsCopy.size(); ++i)
            {
                if (! (pfds[1 + i].revents & POLLIN))
                    continue;

                int fd = clientsCopy[i];
                std::vector<uint8_t> msg;

                if (! glaRecvMessage (fd, msg))
                {
                    syslog (LOG_INFO, "GLA: client disconnected (fd %d)", fd);
                    close (fd);

                    std::lock_guard<std::mutex> lk (clientsMutex);
                    clients.erase (std::remove (clients.begin(), clients.end(), fd), clients.end());
                }
                else
                {
                    handleClientMessage (fd, msg);
                }
            }
        }
    }

    void handleClientMessage (int /*clientFd*/, const std::vector<uint8_t>& msg)
    {
        if (msg.size() < 4)
            return;

        uint32_t type = 0;
        memcpy (&type, msg.data(), 4);
        auto msgType = static_cast<GLAMsgType> (type);

        if (msgType == GLAMsgType::SetRouting && msg.size() >= 4 + 1 + 8)
        {
            uint8_t channelIndex = msg[4];
            uint64_t entityId    = 0;
            memcpy (&entityId, msg.data() + 5, 8);

            if (onSetRouting)
                onSetRouting (channelIndex, entityId);
        }
        else if (msgType == GLAMsgType::SetNetworkInterface && msg.size() > 8)
        {
            uint32_t len = 0;
            memcpy (&len, msg.data() + 4, 4);

            if (msg.size() >= 8 + len)
            {
                std::string iface (reinterpret_cast<const char*> (msg.data() + 8), len);

                if (onSetNetif)
                    onSetNetif (iface);
            }
        }
        else if (msgType == GLAMsgType::SetUSBBridge && msg.size() > 8)
        {
            uint32_t len = 0;
            memcpy (&len, msg.data() + 4, 4);

            if (msg.size() >= 8 + len)
            {
                std::string uid (reinterpret_cast<const char*> (msg.data() + 8), len);

                if (onSetBridge)
                    onSetBridge (uid);
            }
        }
    }

    int serverFd = -1;
    std::atomic<bool> running  { false };
    std::thread thread;
    std::mutex clientsMutex;
    std::vector<int> clients;
    std::string currentBridgeUID;           // guarded by clientsMutex
    std::vector<uint8_t> currentChannelMapMsg; // guarded by clientsMutex

    SetRoutingCallback onSetRouting;
    SetNetifCallback   onSetNetif;
    SetBridgeCallback  onSetBridge;
};
