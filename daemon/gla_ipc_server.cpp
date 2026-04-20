#include "gla_ipc_server.hpp"
#include "../common/gla_socket.hpp"
#include <sys/select.h>
#include <unistd.h>
#include <syslog.h>
#include <algorithm>
#include <cstring>

GLAIPCServer::GLAIPCServer() = default;

GLAIPCServer::~GLAIPCServer() {
    stop();
}

bool GLAIPCServer::start() {
    _serverFd = glaCreateServer(GLA_SOCKET_PATH);
    if (_serverFd < 0) {
        syslog(LOG_ERR, "GLA daemon: failed to create IPC server socket");
        return false;
    }
    _running = true;
    _thread = std::thread([this]{ runLoop(); });
    syslog(LOG_INFO, "GLA daemon: IPC server started at %s", GLA_SOCKET_PATH);
    return true;
}

void GLAIPCServer::stop() {
    _running = false;
    if (_serverFd >= 0) { close(_serverFd); _serverFd = -1; }
    if (_thread.joinable()) _thread.join();
    std::lock_guard<std::mutex> lk(_clientsMutex);
    for (int fd : _clients) close(fd);
    _clients.clear();
}

void GLAIPCServer::runLoop() {
    while (_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(_serverFd, &readfds);
        int maxfd = _serverFd;

        {
            std::lock_guard<std::mutex> lk(_clientsMutex);
            for (int fd : _clients) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) maxfd = fd;
            }
        }

        timeval tv{1, 0}; // 1s timeout so we can check _running
        int ret = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        // Accept new connection.
        if (FD_ISSET(_serverFd, &readfds)) {
            int clientFd = glaAcceptClient(_serverFd);
            if (clientFd >= 0) {
                std::lock_guard<std::mutex> lk(_clientsMutex);
                _clients.push_back(clientFd);
                syslog(LOG_INFO, "GLA daemon: client connected (fd %d)", clientFd);
            }
        }

        // Read from existing clients.
        std::vector<int> clientsCopy;
        {
            std::lock_guard<std::mutex> lk(_clientsMutex);
            clientsCopy = _clients;
        }
        for (int fd : clientsCopy) {
            if (!FD_ISSET(fd, &readfds)) continue;
            std::vector<uint8_t> msg;
            if (!glaRecvMessage(fd, msg)) {
                syslog(LOG_INFO, "GLA daemon: client disconnected (fd %d)", fd);
                close(fd);
                std::lock_guard<std::mutex> lk(_clientsMutex);
                _clients.erase(std::remove(_clients.begin(), _clients.end(), fd),
                               _clients.end());
            } else {
                handleClientMessage(fd, msg);
            }
        }
    }
}

void GLAIPCServer::handleClientMessage(int /*clientFd*/, const std::vector<uint8_t>& msg) {
    if (msg.size() < 4) return;
    uint32_t type = 0;
    memcpy(&type, msg.data(), 4);
    auto msgType = static_cast<GLAMsgType>(type);

    if (msgType == GLAMsgType::SetRouting && msg.size() >= 4 + 1 + 8) {
        uint8_t channelIndex = msg[4];
        uint64_t entityId = 0;
        memcpy(&entityId, msg.data() + 5, 8);
        if (_onSetRouting) _onSetRouting(channelIndex, entityId);
    } else if (msgType == GLAMsgType::SetNetworkInterface && msg.size() > 8) {
        uint32_t len = 0;
        memcpy(&len, msg.data() + 4, 4);
        if (msg.size() >= 8 + len) {
            std::string iface(reinterpret_cast<const char*>(msg.data() + 8), len);
            if (_onSetNetif) _onSetNetif(iface);
        }
    } else if (msgType == GLAMsgType::SetUSBBridge && msg.size() > 8) {
        uint32_t len = 0;
        memcpy(&len, msg.data() + 4, 4);
        if (msg.size() >= 8 + len) {
            std::string uid(reinterpret_cast<const char*>(msg.data() + 8), len);
            if (_onSetBridge) _onSetBridge(uid);
        }
    }
}

void GLAIPCServer::broadcastChannelMap(const std::vector<GLAChannelEntry>& entries) {
    auto payload = serializeChannelMapUpdate(entries);
    std::lock_guard<std::mutex> lk(_clientsMutex);
    std::vector<int> dead;
    for (int fd : _clients) {
        if (!glaSendMessage(fd, payload)) dead.push_back(fd);
    }
    for (int fd : dead) {
        close(fd);
        _clients.erase(std::remove(_clients.begin(), _clients.end(), fd), _clients.end());
    }
}

void GLAIPCServer::broadcastEntityList(const std::vector<GLAEntityInfo>& entities) {
    auto payload = serializeEntityList(entities);
    std::lock_guard<std::mutex> lk(_clientsMutex);
    std::vector<int> dead;
    for (int fd : _clients) {
        if (!glaSendMessage(fd, payload)) dead.push_back(fd);
    }
    for (int fd : dead) {
        close(fd);
        _clients.erase(std::remove(_clients.begin(), _clients.end(), fd), _clients.end());
    }
}
