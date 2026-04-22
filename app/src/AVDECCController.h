
#pragma once

#include <la/avdecc/controller/avdeccController.hpp>
#include <la/avdecc/avdecc.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <syslog.h>
#include <unordered_map>
#include <vector>
#include "../../common/GLAIPCTypes.h"

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
