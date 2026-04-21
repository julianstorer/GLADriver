#include "gla_driver.hpp"
#include "gla_device.hpp"
#include "gla_usb_reader.hpp"
#include "gla_ipc_client.hpp"
#include <aspl/Plugin.hpp>
#include <syslog.h>

// Hardcoded test entries used until the daemon connects and sends a real channel map.
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
    syslog(LOG_INFO, "GLA: Initialize() start");

    OSStatus err = aspl::Driver::Initialize();
    if (err != noErr) {
        syslog(LOG_ERR, "GLA: aspl::Driver::Initialize() failed: %d", (int)err);
        return err;
    }

    syslog(LOG_INFO, "GLA: calling applyChannelMap (test map, %zu entries)",
           kTestChannelMap.size());
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
    syslog(LOG_INFO, "GLA: applyChannelMap(%zu entries)", entries.size());

    auto plugin  = GetPlugin();
    auto context = GetContext();

    if (_unifiedDevice) {
        syslog(LOG_INFO, "GLA: removing old unified device");
        plugin->RemoveDevice(_unifiedDevice);
        _usbReader->clearChannelBuffers(); // before reset() so IOProc never writes to freed rings
        _unifiedDevice.reset();
    }

    if (entries.empty()) {
        syslog(LOG_INFO, "GLA: empty map, no device created");
        return;
    }

    syslog(LOG_INFO, "GLA: creating GLAUnifiedDevice");
    _unifiedDevice = std::make_shared<GLAUnifiedDevice>(context, entries);
    syslog(LOG_INFO, "GLA: calling init()");
    _unifiedDevice->init();
    syslog(LOG_INFO, "GLA: calling AddDevice");
    plugin->AddDevice(_unifiedDevice);
    syslog(LOG_INFO, "GLA: AddDevice done");

    for (const auto& e : entries)
        _usbReader->setChannelBuffer(e.channel_index,
                                     _unifiedDevice->getChannelRingBuffer(e.channel_index));

    syslog(LOG_INFO, "GLA: applied channel map (%zu sources)", entries.size());
}
