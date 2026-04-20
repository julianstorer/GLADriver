#include "gla_ipc_client.hpp"
#include "../common/gla_socket.hpp"
#include <syslog.h>
#include <unistd.h>
#include <chrono>

GLAIPCClient::GLAIPCClient() = default;

GLAIPCClient::~GLAIPCClient() {
    stop();
}

void GLAIPCClient::start(ChannelMapCallback cb) {
    _callback = std::move(cb);
    _running = true;
    _thread = std::thread([this]{ runLoop(); });
}

void GLAIPCClient::stop() {
    _running = false;
    if (_fd >= 0) { close(_fd); _fd = -1; }
    if (_thread.joinable()) _thread.join();
}

void GLAIPCClient::runLoop() {
    int backoff = 1;
    while (_running) {
        if (_fd < 0) {
            if (!tryConnect()) {
                std::this_thread::sleep_for(std::chrono::seconds(backoff));
                backoff = std::min(backoff * 2, 5);
                continue;
            }
            backoff = 1;
            syslog(LOG_INFO, "GLA: connected to daemon");
        }
        std::vector<uint8_t> msg;
        if (!glaRecvMessage(_fd, msg)) {
            syslog(LOG_WARNING, "GLA: lost connection to daemon");
            close(_fd);
            _fd = -1;
            continue;
        }
        handleMessage(msg);
    }
}

bool GLAIPCClient::tryConnect() {
    _fd = glaConnect(GLA_SOCKET_PATH);
    return _fd >= 0;
}

void GLAIPCClient::handleMessage(const std::vector<uint8_t>& msg) {
    if (msg.size() < 8) return;
    uint32_t type = 0;
    uint32_t count = 0;
    memcpy(&type,  msg.data(),     4);
    memcpy(&count, msg.data() + 4, 4);

    if (static_cast<GLAMsgType>(type) != GLAMsgType::ChannelMapUpdate) return;
    if (msg.size() < 8 + count * sizeof(GLAChannelEntry)) return;

    std::vector<GLAChannelEntry> entries(count);
    memcpy(entries.data(), msg.data() + 8, count * sizeof(GLAChannelEntry));
    if (_callback) _callback(entries);
}
