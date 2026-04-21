#include "AppBackend.hpp"
#include <cstring>
#include <juce_events/juce_events.h>
#include <syslog.h>

AppBackend::AppBackend() = default;

AppBackend::~AppBackend()
{
    stop();
}

bool AppBackend::start (const std::string& networkInterface)
{
    ipc.setSetRoutingCallback ([this] (uint8_t ch, uint64_t eid)
    {
        std::string name = "Unknown";
        for (auto const& r : avdecc.getEntities())
            if (r.id == eid) { name = r.name; break; }
        {
            std::lock_guard<std::mutex> lk (mutex);
            bool found = false;
            for (auto& r : routing)
            {
                if (r.channelIndex == ch)
                {
                    r.entityId = eid; r.displayName = name; found = true; break;
                }
            }
            if (!found) routing.push_back ({ ch, eid, name });
        }
        ipc.broadcastChannelMap (buildChannelMap());
    });

    ipc.setSetNetifCallback ([this] (const std::string& iface)
    {
        avdecc.stop();
        avdecc.start (iface);
    });

    ipc.setSetBridgeCallback ([this] (const std::string& uid)
    {
        std::lock_guard<std::mutex> lk (mutex);
        usbBridgeUID = uid;
    });

    avdecc.setOnChangeCallback ([this]()
    {
        auto entityList = buildEntityList();
        auto map        = buildChannelMap();
        ipc.broadcastEntityList (entityList);
        ipc.broadcastChannelMap (map);
        juce::MessageManager::callAsync ([this, entityList = std::move (entityList)]() mutable
        {
            if (onEntityList) onEntityList (entityList);
        });
    });

    if (!ipc.start())
    {
        syslog (LOG_ERR, "GLA: failed to start IPC server");
        return false;
    }

    if (!avdecc.start (networkInterface))
        syslog (LOG_WARNING, "GLA: AVDECC start failed; continuing without AVB discovery");

    return true;
}

void AppBackend::stop()
{
    avdecc.stop();
    ipc.stop();
}

void AppBackend::setNetworkInterface (const std::string& iface)
{
    avdecc.stop();
    avdecc.start (iface);
}

void AppBackend::setRouting (uint8_t channelIndex, uint64_t entityId)
{
    std::string name = "Unknown";
    for (auto const& r : avdecc.getEntities())
        if (r.id == entityId) { name = r.name; break; }
    {
        std::lock_guard<std::mutex> lk (mutex);
        bool found = false;
        for (auto& r : routing)
        {
            if (r.channelIndex == channelIndex)
            {
                r.entityId = entityId; r.displayName = name; found = true; break;
            }
        }
        if (!found) routing.push_back ({ channelIndex, entityId, name });
    }
    ipc.broadcastChannelMap (buildChannelMap());
}

void AppBackend::setUSBBridge (const std::string& uid)
{
    std::lock_guard<std::mutex> lk (mutex);
    usbBridgeUID = uid;
}

std::vector<GLAChannelEntry> AppBackend::buildChannelMap() const
{
    std::vector<GLAChannelEntry> entries;
    std::lock_guard<std::mutex> lk (mutex);
    for (auto const& r : routing)
    {
        GLAChannelEntry e{};
        e.channelIndex = r.channelIndex;
        e.entityId     = r.entityId;
        strncpy (e.displayName, r.displayName.c_str(), sizeof (e.displayName) - 1);
        entries.push_back (e);
    }
    return entries;
}

std::vector<GLAEntityInfo> AppBackend::buildEntityList() const
{
    auto records = avdecc.getEntities();
    std::vector<GLAEntityInfo> list;
    for (auto const& r : records)
    {
        GLAEntityInfo info{};
        info.entityId    = r.id;
        info.online      = r.online;
        info.streamCount = 0;
        strncpy (info.name, r.name.c_str(), sizeof (info.name) - 1);
        list.push_back (info);
    }
    return list;
}
