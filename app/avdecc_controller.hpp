#pragma once
#include <la/avdecc/controller/avdeccController.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../common/gla_ipc_types.hpp"

//==============================================================================
struct EntityRecord
{
    uint64_t    id;
    std::string name;
    bool        online;
};

//==============================================================================
class AVDECCController : public la::avdecc::controller::Controller::DefaultedObserver
{
public:
    using OnChangeCallback = std::function<void()>;

    AVDECCController();
    ~AVDECCController();

    //==============================================================================
    // Start discovery on the given network interface (e.g. "en6").
    bool start (const std::string& networkInterface);
    void stop();

    // Thread-safe snapshot of all known entities.
    std::vector<EntityRecord> getEntities() const;

    void setOnChangeCallback (OnChangeCallback cb) { onChange = std::move (cb); }

private:
    //==============================================================================
    void onEntityOnline (la::avdecc::controller::Controller const*,
                         la::avdecc::controller::ControlledEntity const* entity) noexcept override;
    void onEntityOffline (la::avdecc::controller::Controller const*,
                          la::avdecc::controller::ControlledEntity const* entity) noexcept override;
    void onEntityNameChanged (la::avdecc::controller::Controller const*,
                              la::avdecc::controller::ControlledEntity const*,
                              la::avdecc::entity::model::AvdeccFixedString const&) noexcept override;

    la::avdecc::controller::Controller::UniquePointer controller { nullptr, [] (la::avdecc::controller::Controller*) {} };
    mutable std::mutex                                mutex;
    std::unordered_map<uint64_t, EntityRecord>        entities;
    OnChangeCallback                                  onChange;
};
