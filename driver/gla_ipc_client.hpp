#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include "../common/gla_ipc_types.hpp"

// Non-blocking IPC client that connects to the app backend.
// Runs a background thread that receives ChannelMapUpdate messages
// and calls the provided callback on the control thread.
class GLAIPCClient
{
public:
    using ChannelMapCallback = std::function<void (const std::vector<GLAChannelEntry>&)>;

    GLAIPCClient();
    ~GLAIPCClient();

    void start (ChannelMapCallback cb);
    void stop();

private:
    void runLoop();
    bool tryConnect();
    void handleMessage (const std::vector<uint8_t>& msg);

    int                  fd      = -1;
    std::atomic<bool>    running { false };
    std::thread          thread;
    ChannelMapCallback   callback;
};
