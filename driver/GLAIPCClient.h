#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../common/GLAIPCTypes.h"
#include "../common/GLASocket.h"

class GLAIPCClient
{
public:
    GLAIPCClient() = default;
    ~GLAIPCClient()  { stop(); }

    using ChannelMapCallback = std::function<void(const std::vector<GLAChannelEntry>&)>;

    void start (ChannelMapCallback cb)
    {
        callback = std::move (cb);
        running = true;
        thread = std::thread ([this] { runLoop(); });
    }

    void stop()
    {
        running = false;

        if (fd >= 0)
        {
            close (fd);
            fd = -1;
        }

        if (thread.joinable())
            thread.join();
    }

private:
    void runLoop()
    {
        int backoff = 1;

        while (running)
        {
            if (fd < 0)
            {
                if (! tryConnect())
                {
                    std::this_thread::sleep_for (std::chrono::seconds (backoff));
                    backoff = std::min (backoff * 2, 5);
                    continue;
                }

                backoff = 1;
                syslog (LOG_INFO, "GLA: connected to app");
            }

            std::vector<uint8_t> msg;

            if (! glaRecvMessage (fd, msg))
            {
                syslog (LOG_WARNING, "GLA: lost connection to app");
                close (fd);
                fd = -1;
                continue;
            }

            handleMessage (msg);
        }
    }

    bool tryConnect()
    {
        fd = glaConnect (glaSocketPath);
        return fd >= 0;
    }

    void handleMessage (const std::vector<uint8_t>& msg)
    {
        if (msg.size() < 8)
            return;

        uint32_t type  = 0;
        uint32_t count = 0;
        memcpy (&type,  msg.data(),     4);
        memcpy (&count, msg.data() + 4, 4);

        if (static_cast<GLAMsgType> (type) != GLAMsgType::ChannelMapUpdate)
            return;

        if (msg.size() < 8 + count * sizeof (GLAChannelEntry))
            return;

        std::vector<GLAChannelEntry> entries (count);
        memcpy (entries.data(), msg.data() + 8, count * sizeof (GLAChannelEntry));

        if (callback)
            callback (entries);
    }

    int fd = -1;
    std::atomic<bool> running { false };
    std::thread thread;
    ChannelMapCallback callback;
};
