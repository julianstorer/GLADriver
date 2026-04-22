#include "gla_ipc_client.hpp"
#include "../common/gla_socket.hpp"
#include <chrono>
#include <syslog.h>
#include <unistd.h>

GLAIPCClient::GLAIPCClient() = default;

GLAIPCClient::~GLAIPCClient()
{
    stop();
}

void GLAIPCClient::start (ChannelMapCallback cb)
{
    callback = std::move (cb);
    running  = true;
    thread   = std::thread ([this] { runLoop(); });
}

void GLAIPCClient::stop()
{
    running = false;
    if (fd >= 0) { close (fd); fd = -1; }
    if (thread.joinable()) thread.join();
}

void GLAIPCClient::runLoop()
{
    int backoff = 1;
    while (running)
    {
        if (fd < 0)
        {
            if (!tryConnect())
            {
                std::this_thread::sleep_for (std::chrono::seconds (backoff));
                backoff = std::min (backoff * 2, 5);
                continue;
            }
            backoff = 1;
            syslog (LOG_INFO, "GLA: connected to app");
        }
        std::vector<uint8_t> msg;
        if (!glaRecvMessage (fd, msg))
        {
            syslog (LOG_WARNING, "GLA: lost connection to app");
            close (fd);
            fd = -1;
            continue;
        }
        handleMessage (msg);
    }
}

bool GLAIPCClient::tryConnect()
{
    fd = glaConnect (glaSocketPath);
    return fd >= 0;
}

void GLAIPCClient::handleMessage (const std::vector<uint8_t>& msg)
{
    if (msg.size() < 8) return;
    uint32_t type  = 0;
    uint32_t count = 0;
    memcpy (&type,  msg.data(),     4);
    memcpy (&count, msg.data() + 4, 4);

    if (static_cast<GLAMsgType> (type) != GLAMsgType::ChannelMapUpdate) return;
    if (msg.size() < 8 + count * sizeof (GLAChannelEntry)) return;

    std::vector<GLAChannelEntry> entries (count);
    memcpy (entries.data(), msg.data() + 8, count * sizeof (GLAChannelEntry));
    if (callback) callback (entries);
}
