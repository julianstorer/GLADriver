#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdint>
#include "../common/gla_ipc_types.hpp"

// Non-blocking IPC client that connects to gla-daemon.
// Runs a background thread that receives ChannelMapUpdate messages
// and calls the provided callback on the control thread.
class GLAIPCClient {
public:
    using ChannelMapCallback = std::function<void(const std::vector<GLAChannelEntry>&)>;

    GLAIPCClient();
    ~GLAIPCClient();

    void start(ChannelMapCallback cb);
    void stop();

private:
    void runLoop();
    bool tryConnect();
    void handleMessage(const std::vector<uint8_t>& msg);

    int _fd = -1;
    std::atomic<bool> _running{false};
    std::thread _thread;
    ChannelMapCallback _callback;
};
