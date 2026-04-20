#include "avdecc_controller.hpp"
#include "usb_bridge_monitor.hpp"
#include "gla_ipc_server.hpp"
#include "../common/gla_ipc_types.hpp"
#include <csignal>
#include <syslog.h>
#include <unistd.h>
#include <mutex>

static volatile bool gRunning = true;

static void sigHandler(int) { gRunning = false; }

// Minimal config: maps channel_index -> entity_id
struct RoutingEntry {
    uint8_t  channelIndex;
    uint64_t entityId;
    std::string displayName;
};

struct Config {
    std::string networkInterface;
    std::string usbBridgeUID;
    std::vector<RoutingEntry> routing;
};

// Globals shared across callbacks.
static Config gConfig;
static AVDECCController gAVDECC;
static USBBridgeMonitor gBridgeMon;
static GLAIPCServer gIPC;
static std::mutex gConfigMutex;

static std::vector<GLAChannelEntry> buildChannelMap() {
    std::vector<GLAChannelEntry> entries;
    std::lock_guard<std::mutex> lk(gConfigMutex);
    for (auto const& r : gConfig.routing) {
        GLAChannelEntry e{};
        e.channel_index = r.channelIndex;
        e.entity_id     = r.entityId;
        strncpy(e.display_name, r.displayName.c_str(), sizeof(e.display_name) - 1);
        entries.push_back(e);
    }
    return entries;
}

static std::vector<GLAEntityInfo> buildEntityList() {
    auto records = gAVDECC.getEntities();
    std::vector<GLAEntityInfo> list;
    for (auto const& r : records) {
        GLAEntityInfo info{};
        info.entity_id   = r.id;
        info.online      = r.online;
        info.stream_count = 0;
        strncpy(info.name, r.name.c_str(), sizeof(info.name) - 1);
        list.push_back(info);
    }
    return list;
}

int main(int argc, char* argv[]) {
    openlog("gla-daemon", LOG_PID, LOG_DAEMON);
    signal(SIGTERM, sigHandler);
    signal(SIGINT,  sigHandler);

    // Parse --interface argument.
    std::string netif = "en6";
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--interface") netif = argv[i+1];
    }

    gConfig.networkInterface = netif;

    // IPC server.
    gIPC.setSetRoutingCallback([](uint8_t ch, uint64_t eid) {
        syslog(LOG_INFO, "GLA daemon: routing ch %d -> entity 0x%llx",
               ch, (unsigned long long)eid);
        // Find entity name.
        std::string name = "Unknown";
        for (auto const& r : gAVDECC.getEntities()) {
            if (r.id == eid) { name = r.name; break; }
        }
        {
            std::lock_guard<std::mutex> lk(gConfigMutex);
            bool found = false;
            for (auto& r : gConfig.routing) {
                if (r.channelIndex == ch) {
                    r.entityId = eid; r.displayName = name; found = true; break;
                }
            }
            if (!found) gConfig.routing.push_back({ch, eid, name});
        }
        gIPC.broadcastChannelMap(buildChannelMap());
    });

    gIPC.setSetNetifCallback([](const std::string& iface) {
        syslog(LOG_INFO, "GLA daemon: switching to interface '%s'", iface.c_str());
        gAVDECC.stop();
        gAVDECC.start(iface);
    });

    gIPC.setSetBridgeCallback([](const std::string& uid) {
        syslog(LOG_INFO, "GLA daemon: USB bridge selected: '%s'", uid.c_str());
        std::lock_guard<std::mutex> lk(gConfigMutex);
        gConfig.usbBridgeUID = uid;
        // HAL driver will pick this up via config or a dedicated message.
        // TODO: send SetUSBBridge message to HAL driver.
    });

    gAVDECC.setOnChangeCallback([]() {
        gIPC.broadcastEntityList(buildEntityList());
        gIPC.broadcastChannelMap(buildChannelMap());
    });

    if (!gIPC.start()) {
        syslog(LOG_ERR, "GLA daemon: failed to start IPC server, exiting");
        return 1;
    }

    if (!gAVDECC.start(netif)) {
        syslog(LOG_WARNING, "GLA daemon: AVDECC start failed; continuing without AVB discovery");
    }

    syslog(LOG_INFO, "GLA daemon: running");

    while (gRunning) sleep(1);

    syslog(LOG_INFO, "GLA daemon: shutting down");
    gAVDECC.stop();
    gIPC.stop();
    closelog();
    return 0;
}
