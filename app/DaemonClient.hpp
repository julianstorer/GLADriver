#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "../common/gla_ipc_types.hpp"
#include <functional>
#include <vector>
#include <string>

// Asynchronous client to gla-daemon.
// Receives entity list and channel map updates on the message thread.
class DaemonClient : public juce::Thread {
public:
    using EntityListCallback   = std::function<void(const std::vector<GLAEntityInfo>&)>;
    using ChannelMapCallback   = std::function<void(const std::vector<GLAChannelEntry>&)>;

    DaemonClient();
    ~DaemonClient() override;

    void setEntityListCallback(EntityListCallback cb) { _onEntityList = std::move(cb); }
    void setChannelMapCallback(ChannelMapCallback cb) { _onChannelMap = std::move(cb); }

    void run() override;

    // Send commands to daemon.
    void sendSetRouting(uint8_t channelIndex, uint64_t entityId);
    void sendSetNetworkInterface(const std::string& iface);
    void sendSetUSBBridge(const std::string& uid);
    void sendGetEntityList();

private:
    bool tryConnect();
    void handleMessage(const std::vector<uint8_t>& msg);

    int _fd = -1;
    EntityListCallback _onEntityList;
    ChannelMapCallback _onChannelMap;
};
