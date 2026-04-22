
#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <sys/select.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../../common/gla_ipc_types.hpp"
#include "../../common/gla_socket.hpp"

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

private:
    //==============================================================================
    void runLoop()
    {
        while (running)
        {
            fd_set readfds;
            FD_ZERO (&readfds);
            FD_SET (serverFd, &readfds);
            int maxfd = serverFd;

            {
                std::lock_guard<std::mutex> lk (clientsMutex);

                for (int fd : clients)
                {
                    FD_SET (fd, &readfds);

                    if (fd > maxfd)
                        maxfd = fd;
                }
            }

            timeval tv { 1, 0 }; // 1s timeout so we can check running
            int ret = select (maxfd + 1, &readfds, nullptr, nullptr, &tv);

            if (ret <= 0)
                continue;

            if (FD_ISSET (serverFd, &readfds))
            {
                if (int clientFd = glaAcceptClient (serverFd); clientFd >= 0)
                {
                    std::lock_guard<std::mutex> lk (clientsMutex);
                    clients.push_back (clientFd);
                    syslog (LOG_INFO, "GLA: client connected (fd %d)", clientFd);
                }
            }

            std::vector<int> clientsCopy;

            {
                std::lock_guard<std::mutex> lk (clientsMutex);
                clientsCopy = clients;
            }

            for (int fd : clientsCopy)
            {
                if (!FD_ISSET (fd, &readfds))
                    continue;

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

    SetRoutingCallback onSetRouting;
    SetNetifCallback   onSetNetif;
    SetBridgeCallback  onSetBridge;
};
