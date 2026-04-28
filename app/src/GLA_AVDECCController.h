
#pragma once

#include <la/avdecc/controller/avdeccController.hpp>
#include <la/avdecc/avdecc.hpp>
#include <la/avdecc/internals/streamFormatInfo.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <syslog.h>
#include <unordered_map>
#include <vector>
#include "../../common/GLA_IPCTypes.h"

//==============================================================================
struct EntityRecord
{
    uint64_t    id;
    std::string name;
    bool        online;
};

//==============================================================================
class AVDECCController  : public la::avdecc::controller::Controller::DefaultedObserver
{
public:
    using OnChangeCallback = std::function<void()>;

    AVDECCController() {}
    ~AVDECCController() { stop(); }

    //==============================================================================
    bool start (const std::string& networkInterface)
    {
        stop();
        try
        {
            controller = la::avdecc::controller::Controller::create (
                la::avdecc::protocol::ProtocolInterface::Type::MacOSNative,
                networkInterface,
                /*progID=*/0x0001,
                la::avdecc::UniqueIdentifier{},
                /*preferedLocale=*/"en-US",
                /*entityModelTree=*/nullptr,
                /*executorName=*/std::nullopt,
                /*virtualEntityInterface=*/nullptr);
        }
        catch (la::avdecc::controller::Controller::Exception const& e)
        {
            syslog (LOG_ERR, "GLA: AVDECC controller init failed: %s", e.what());
            return false;
        }
        catch (std::exception const& e)
        {
            syslog (LOG_ERR, "GLA: AVDECC controller init failed: %s", e.what());
            return false;
        }
        catch (...)
        {
            syslog (LOG_ERR, "GLA: AVDECC controller init failed (unknown exception)");
            return false;
        }

        controller->registerObserver (this);

        syslog (LOG_INFO, "GLA: AVDECC controller started on '%s'",
                networkInterface.c_str());

        return true;
    }

    void stop()
    {
        if (controller)
        {
            controller->unregisterObserver (this);
            controller.reset();
        }
    }

    std::vector<EntityRecord> getEntities() const
    {
        std::lock_guard<std::mutex> lk (mutex);
        std::vector<EntityRecord> result;
        result.reserve (entities.size());

        for (auto const& [id, rec] : entities)
            result.push_back (rec);

        return result;
    }

    uint64_t findEntityIdByName (const std::string& name) const
    {
        auto toLower = [] (std::string s) {
            for (auto& c : s) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
            return s;
        };
        std::string nameLower = toLower (name);

        std::lock_guard<std::mutex> lk (mutex);
        for (auto const& [id, rec] : entities)
            if (toLower (rec.name) == nameLower) return id;
        return 0;
    }

    struct ListenerStreamConnection
    {
        uint8_t  streamIndex;
        uint64_t talkerEntityId; // 0 = not connected
        int      channelCount;   // channels in this stream (from stream format, default 2)
    };

    // Returns one entry per input stream on the listener, including unconnected streams.
    // Uses talker-side output connection lists — more reliable than getSinkConnectionInformation.
    std::vector<ListenerStreamConnection> getListenerConnections (uint64_t listenerEntityId) const
    {
        if (! controller || ! listenerEntityId) return {};
        std::vector<ListenerStreamConnection> result;

        // Step 1: enumerate the listener's stream inputs and read channel count from stream format
        try
        {
            auto guard = controller->getControlledEntityGuard (la::avdecc::UniqueIdentifier { listenerEntityId });
            if (! guard) return {};
            auto const& configNode = guard->getCurrentConfigurationNode();
            for (auto const& [streamIdx, streamNode] : configNode.streamInputs)
            {
                auto fi = la::avdecc::entity::model::StreamFormatInfo::create (streamNode.dynamicModel.streamFormat);

                // Skip CRF streams — they carry clock reference data, not audio channels.
                if (fi && fi->getType() == la::avdecc::entity::model::StreamFormatInfo::Type::ClockReference)
                    continue;

                int channelCount = 2; // Milan stereo default
                if (fi && fi->getChannelsCount() > 0)
                    channelCount = static_cast<int> (fi->getChannelsCount());
                result.push_back ({ static_cast<uint8_t> (streamIdx), 0, channelCount });
            }
        }
        catch (...) { return {}; }

        if (result.empty()) return result;

        // Step 2: look at every entity's stream outputs to find connections pointing at our listener.
        // This is more reliable than querying the listener's own connection state (getSinkConnectionInformation
        // can return stale/wrong data, especially for Milan fast-connect and some UAC2 bridge devices).
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk (mutex);
            for (auto const& [id, _] : entities)
                ids.push_back (id);
        }

        for (auto tid : ids)
        {
            try
            {
                auto guard = controller->getControlledEntityGuard (la::avdecc::UniqueIdentifier { tid });
                if (! guard) continue;
                auto const& configNode = guard->getCurrentConfigurationNode();
                for (auto const& [outIdx, _] : configNode.streamOutputs)
                {
                    for (auto const& conn : guard->getStreamOutputConnections (outIdx))
                    {
                        if (conn.entityID.getValue() == listenerEntityId)
                        {
                            auto lStreamIdx = static_cast<uint8_t> (conn.streamIndex);
                            for (auto& r : result)
                                if (r.streamIndex == lStreamIdx) { r.talkerEntityId = tid; break; }
                        }
                    }
                }
            }
            catch (...) {}
        }

        std::sort (result.begin(), result.end(),
                   [] (auto const& a, auto const& b) { return a.streamIndex < b.streamIndex; });
        return result;
    }

    std::set<uint64_t> getConnectedTalkersForListener (uint64_t listenerEntityId) const
    {
        if (! controller || ! listenerEntityId) return {};
        std::set<uint64_t> result;
        try
        {
            auto guard = controller->getControlledEntityGuard (la::avdecc::UniqueIdentifier { listenerEntityId });
            if (! guard) return {};
            auto const& configNode = guard->getCurrentConfigurationNode();
            for (auto const& [streamIdx, _] : configNode.streamInputs)
            {
                auto const& connInfo = guard->getSinkConnectionInformation (streamIdx);
                if (connInfo.state == la::avdecc::entity::model::StreamInputConnectionInfo::State::Connected)
                    result.insert (connInfo.talkerStream.entityID.getValue());
            }
        }
        catch (...) {}
        return result;
    }

    std::set<uint64_t> getActiveTalkerEntityIds() const
    {
        if (! controller) return {};

        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk (mutex);
            ids.reserve (entities.size());
            for (auto const& [id, rec] : entities)
                ids.push_back (id);
        }

        std::set<uint64_t> result;
        for (auto id : ids)
        {
            try
            {
                auto guard = controller->getControlledEntityGuard (la::avdecc::UniqueIdentifier { id });
                if (! guard) continue;
                auto const& configNode = guard->getCurrentConfigurationNode();
                for (auto const& [streamIdx, _] : configNode.streamOutputs)
                {
                    if (! guard->getStreamOutputConnections (streamIdx).empty())
                    {
                        result.insert (id);
                        break;
                    }
                }
            }
            catch (...) {}
        }
        return result;
    }

    void setOnChangeCallback (OnChangeCallback cb) { onChange = std::move (cb); }

private:
    //==============================================================================
    void onEntityOnline (la::avdecc::controller::Controller const*,
                         la::avdecc::controller::ControlledEntity const* entity) noexcept override
    {
        auto const eid = entity->getEntity().getEntityID().getValue();
        std::string name;

        try
        {
            name = entity->getEntityNode().dynamicModel.entityName.str();
        }
        catch (...)
        {
            name = "Unknown Entity";
        }

        {
            std::lock_guard<std::mutex> lk (mutex);
            entities[eid] = EntityRecord { eid, name, true };
        }

        syslog (LOG_INFO, "GLA: entity online: '%s' (0x%llx)",
                name.c_str(), (unsigned long long) eid);

        if (onChange)
            onChange();
    }

    void onEntityOffline (la::avdecc::controller::Controller const*,
                          la::avdecc::controller::ControlledEntity const* entity) noexcept override
    {
        auto const eid = entity->getEntity().getEntityID().getValue();

        {
            std::lock_guard<std::mutex> lk (mutex);

            if (auto it = entities.find (eid); it != entities.end())
                it->second.online = false;
        }

        syslog (LOG_INFO, "GLA: entity offline: 0x%llx", (unsigned long long) eid);

        if (onChange)
            onChange();
    }

    void onStreamInputConnectionChanged (la::avdecc::controller::Controller const*,
                                         la::avdecc::controller::ControlledEntity const*,
                                         la::avdecc::entity::model::StreamIndex const,
                                         la::avdecc::entity::model::StreamInputConnectionInfo const&,
                                         bool) noexcept override
    {
        if (onChange) onChange();
    }

    void onStreamOutputConnectionsChanged (la::avdecc::controller::Controller const*,
                                           la::avdecc::controller::ControlledEntity const*,
                                           la::avdecc::entity::model::StreamIndex const,
                                           la::avdecc::entity::model::StreamConnections const&) noexcept override
    {
        if (onChange) onChange();
    }

    void onEntityNameChanged (la::avdecc::controller::Controller const*,
                              la::avdecc::controller::ControlledEntity const* entity,
                              la::avdecc::entity::model::AvdeccFixedString const& entityName) noexcept override
    {
        auto const eid = entity->getEntity().getEntityID().getValue();
        std::string name = entityName.str();

        {
            std::lock_guard<std::mutex> lk (mutex);

            if (auto it = entities.find (eid); it != entities.end())
                it->second.name = name;
        }

        syslog (LOG_INFO, "GLA: entity renamed: '%s' (0x%llx)",
                name.c_str(), (unsigned long long) eid);

        if (onChange)
            onChange();
    }

    la::avdecc::controller::Controller::UniquePointer controller { nullptr, [] (la::avdecc::controller::Controller*) {} };
    mutable std::mutex                                mutex;
    std::unordered_map<uint64_t, EntityRecord>        entities;
    OnChangeCallback                                  onChange;
};
