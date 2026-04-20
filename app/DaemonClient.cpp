#include "DaemonClient.hpp"
#include "../common/gla_socket.hpp"
#include <unistd.h>
#include <cstring>

DaemonClient::DaemonClient() : juce::Thread("GLA DaemonClient") {
    startThread();
}

DaemonClient::~DaemonClient() {
    signalThreadShouldExit();
    if (_fd >= 0) { close(_fd); _fd = -1; }
    stopThread(3000);
}

void DaemonClient::run() {
    int backoff = 1;
    while (!threadShouldExit()) {
        if (_fd < 0) {
            if (!tryConnect()) {
                wait(backoff * 1000);
                backoff = std::min(backoff * 2, 5);
                continue;
            }
            backoff = 1;
        }
        std::vector<uint8_t> msg;
        if (!glaRecvMessage(_fd, msg)) {
            close(_fd);
            _fd = -1;
            continue;
        }
        handleMessage(msg);
    }
}

bool DaemonClient::tryConnect() {
    _fd = glaConnect(GLA_SOCKET_PATH);
    if (_fd < 0) return false;
    sendGetEntityList();
    return true;
}

void DaemonClient::handleMessage(const std::vector<uint8_t>& msg) {
    if (msg.size() < 8) return;
    uint32_t type = 0, count = 0;
    memcpy(&type,  msg.data(),     4);
    memcpy(&count, msg.data() + 4, 4);

    auto msgType = static_cast<GLAMsgType>(type);

    if (msgType == GLAMsgType::EntityListResponse) {
        if (msg.size() < 8 + count * sizeof(GLAEntityInfo)) return;
        std::vector<GLAEntityInfo> entities(count);
        memcpy(entities.data(), msg.data() + 8, count * sizeof(GLAEntityInfo));
        juce::MessageManager::callAsync([this, entities = std::move(entities)]() mutable {
            if (_onEntityList) _onEntityList(entities);
        });
    } else if (msgType == GLAMsgType::ChannelMapUpdate) {
        if (msg.size() < 8 + count * sizeof(GLAChannelEntry)) return;
        std::vector<GLAChannelEntry> entries(count);
        memcpy(entries.data(), msg.data() + 8, count * sizeof(GLAChannelEntry));
        juce::MessageManager::callAsync([this, entries = std::move(entries)]() mutable {
            if (_onChannelMap) _onChannelMap(entries);
        });
    }
}

void DaemonClient::sendSetRouting(uint8_t channelIndex, uint64_t entityId) {
    std::vector<uint8_t> msg(4 + 1 + 8);
    uint32_t type = static_cast<uint32_t>(GLAMsgType::SetRouting);
    memcpy(msg.data(),     &type,        4);
    msg[4] = channelIndex;
    memcpy(msg.data() + 5, &entityId,    8);
    if (_fd >= 0) glaSendMessage(_fd, msg);
}

void DaemonClient::sendSetNetworkInterface(const std::string& iface) {
    uint32_t type = static_cast<uint32_t>(GLAMsgType::SetNetworkInterface);
    uint32_t len  = static_cast<uint32_t>(iface.size());
    std::vector<uint8_t> msg(8 + len);
    memcpy(msg.data(),     &type, 4);
    memcpy(msg.data() + 4, &len,  4);
    memcpy(msg.data() + 8, iface.data(), len);
    if (_fd >= 0) glaSendMessage(_fd, msg);
}

void DaemonClient::sendSetUSBBridge(const std::string& uid) {
    uint32_t type = static_cast<uint32_t>(GLAMsgType::SetUSBBridge);
    uint32_t len  = static_cast<uint32_t>(uid.size());
    std::vector<uint8_t> msg(8 + len);
    memcpy(msg.data(),     &type, 4);
    memcpy(msg.data() + 4, &len,  4);
    memcpy(msg.data() + 8, uid.data(), len);
    if (_fd >= 0) glaSendMessage(_fd, msg);
}

void DaemonClient::sendGetEntityList() {
    uint32_t type = static_cast<uint32_t>(GLAMsgType::GetEntityList);
    uint32_t zero = 0;
    std::vector<uint8_t> msg(8);
    memcpy(msg.data(),     &type, 4);
    memcpy(msg.data() + 4, &zero, 4);
    if (_fd >= 0) glaSendMessage(_fd, msg);
}
