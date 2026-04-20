#include "avdecc_controller.hpp"
#include <la/avdecc/avdecc.hpp>
#include <syslog.h>

AVDECCController::AVDECCController() {}

AVDECCController::~AVDECCController() {
    stop();
}

bool AVDECCController::start(const std::string& networkInterface) {
    stop();
    try {
        _controller = la::avdecc::controller::Controller::create(
            la::avdecc::protocol::ProtocolInterface::Type::MacOSNative,
            networkInterface,
            /*progID=*/0x0001,
            la::avdecc::UniqueIdentifier{},
            /*preferedLocale=*/"en-US",
            /*entityModelTree=*/nullptr,
            /*executorName=*/std::nullopt,
            /*virtualEntityInterface=*/nullptr);
    } catch (std::exception const& e) {
        syslog(LOG_ERR, "GLA daemon: failed to create AVDECC controller: %s", e.what());
        return false;
    }
    _controller->registerObserver(this);
    syslog(LOG_INFO, "GLA daemon: AVDECC controller started on '%s'",
           networkInterface.c_str());
    return true;
}

void AVDECCController::stop() {
    if (_controller) {
        _controller->unregisterObserver(this);
        _controller.reset();
    }
}

std::vector<EntityRecord> AVDECCController::getEntities() const {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<EntityRecord> result;
    result.reserve(_entities.size());
    for (auto const& [id, rec] : _entities)
        result.push_back(rec);
    return result;
}

void AVDECCController::onEntityOnline(
    la::avdecc::controller::Controller const*,
    la::avdecc::controller::ControlledEntity const* entity) noexcept
{
    auto const eid = entity->getEntity().getEntityID().getValue();
    std::string name;
    try {
        name = entity->getEntityNode().dynamicModel.entityName.str();
    } catch (...) {
        name = "Unknown Entity";
    }

    {
        std::lock_guard<std::mutex> lk(_mutex);
        _entities[eid] = EntityRecord{eid, name, true};
    }
    syslog(LOG_INFO, "GLA daemon: entity online: '%s' (0x%llx)",
           name.c_str(), (unsigned long long)eid);
    if (_onChange) _onChange();
}

void AVDECCController::onEntityOffline(
    la::avdecc::controller::Controller const*,
    la::avdecc::controller::ControlledEntity const* entity) noexcept
{
    auto const eid = entity->getEntity().getEntityID().getValue();
    {
        std::lock_guard<std::mutex> lk(_mutex);
        auto it = _entities.find(eid);
        if (it != _entities.end())
            it->second.online = false;
    }
    syslog(LOG_INFO, "GLA daemon: entity offline: 0x%llx", (unsigned long long)eid);
    if (_onChange) _onChange();
}

void AVDECCController::onEntityNameChanged(
    la::avdecc::controller::Controller const*,
    la::avdecc::controller::ControlledEntity const* entity,
    la::avdecc::entity::model::AvdeccFixedString const& entityName) noexcept
{
    auto const eid = entity->getEntity().getEntityID().getValue();
    std::string name = entityName.str();
    {
        std::lock_guard<std::mutex> lk(_mutex);
        auto it = _entities.find(eid);
        if (it != _entities.end())
            it->second.name = name;
    }
    syslog(LOG_INFO, "GLA daemon: entity renamed: '%s' (0x%llx)",
           name.c_str(), (unsigned long long)eid);
    if (_onChange) _onChange();
}
