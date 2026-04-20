#include "gla_driver.hpp"
#include "gla_device.hpp"
#include "gla_usb_reader.hpp"
#include "gla_ipc_client.hpp"
#include <aspl/Plugin.hpp>
#include <syslog.h>

// Hardcoded test entries used until the daemon connects and sends a real channel map.
// These let us verify the HAL driver appears in CoreAudio with named channels.
static const std::vector<GLAChannelEntry> kTestChannelMap = []() {
    std::vector<GLAChannelEntry> v(2);
    v[0].channel_index = 0;
    v[0].entity_id     = 0x0000000000000001ULL;
    snprintf(v[0].display_name, sizeof(v[0].display_name), "Bob's Guitar");
    v[1].channel_index = 1;
    v[1].entity_id     = 0x0000000000000002ULL;
    snprintf(v[1].display_name, sizeof(v[1].display_name), "Alice's Vocals");
    return v;
}();

GLADriver::GLADriver()
    : aspl::Driver()
    , _usbReader(std::make_shared<GLAUSBReader>())
    , _ipcClient(std::make_shared<GLAIPCClient>())
{
}

OSStatus GLADriver::Initialize()
{
    OSStatus err = aspl::Driver::Initialize();
    if (err != noErr)
        return err;

    applyChannelMap(kTestChannelMap);

    _ipcClient->start([this](const std::vector<GLAChannelEntry>& entries) {
        applyChannelMap(entries);
    });

    syslog(LOG_INFO, "GLA: driver initialized");
    return noErr;
}

GLADriver::~GLADriver() {
    _ipcClient->stop();
    _usbReader->stop();
}

void GLADriver::applyChannelMap(const std::vector<GLAChannelEntry>& entries) {
    auto plugin  = GetPlugin();
    auto context = GetContext();

    // Build the next set of devices.
    std::unordered_map<uint64_t, std::shared_ptr<GLAEntityDevice>> next;
    for (const auto& e : entries) next[e.entity_id] = nullptr;

    // Remove devices that are no longer in the map.
    for (auto& [eid, dev] : _devices) {
        if (next.find(eid) == next.end()) {
            plugin->RemoveDevice(dev);
            syslog(LOG_INFO, "GLA: removed device for entity 0x%llx",
                   (unsigned long long)eid);
        }
    }

    // Add or reuse devices, update USB mapping.
    _usbReader->clearChannelDevices();
    for (const auto& e : entries) {
        std::string name(e.display_name);
        auto it = _devices.find(e.entity_id);
        if (it != _devices.end()) {
            next[e.entity_id] = it->second;
        } else {
            auto dev = std::make_shared<GLAEntityDevice>(
                context, name, e.entity_id, e.channel_index);
            dev->init();
            plugin->AddDevice(dev);
            next[e.entity_id] = dev;
        }
        _usbReader->setChannelDevice(e.channel_index, next[e.entity_id].get());
    }

    _devices = std::move(next);
}
