#include "gla_driver.hpp"
#include "gla_device.hpp"
#include "gla_usb_reader.hpp"
#include "gla_ipc_client.hpp"
#include <aspl/Plugin.hpp>
#include <syslog.h>

// Hardcoded test entries used until the app connects and sends a real channel map.
static const std::vector<GLAChannelEntry> testChannelMap = []()
{
    std::vector<GLAChannelEntry> v (2);
    v[0].channelIndex = 0;
    v[0].entityId     = 0x0000000000000001ULL;
    snprintf (v[0].displayName, sizeof (v[0].displayName), "Bob's Guitar");
    v[1].channelIndex = 1;
    v[1].entityId     = 0x0000000000000002ULL;
    snprintf (v[1].displayName, sizeof (v[1].displayName), "Alice's Vocals");
    return v;
}();

GLADriver::GLADriver()
    : aspl::Driver()
    , usbReader (std::make_shared<GLAUSBReader>())
    , ipcClient (std::make_shared<GLAIPCClient>())
{
}

OSStatus GLADriver::Initialize()
{
    syslog (LOG_INFO, "GLA: Initialize() start");

    OSStatus err = aspl::Driver::Initialize();
    if (err != noErr)
    {
        syslog (LOG_ERR, "GLA: aspl::Driver::Initialize() failed: %d", (int) err);
        return err;
    }

    syslog (LOG_INFO, "GLA: calling applyChannelMap (test map, %zu entries)",
            testChannelMap.size());
    applyChannelMap (testChannelMap);

    ipcClient->start ([this] (const std::vector<GLAChannelEntry>& entries)
    {
        applyChannelMap (entries);
    });

    syslog (LOG_INFO, "GLA: driver initialized");
    return noErr;
}

GLADriver::~GLADriver()
{
    ipcClient->stop();
    usbReader->stop();
}

void GLADriver::applyChannelMap (const std::vector<GLAChannelEntry>& entries)
{
    syslog (LOG_INFO, "GLA: applyChannelMap(%zu entries)", entries.size());

    auto plugin  = GetPlugin();
    auto context = GetContext();

    if (unifiedDevice)
    {
        syslog (LOG_INFO, "GLA: removing old unified device");
        plugin->RemoveDevice (unifiedDevice);
        usbReader->clearChannelBuffers();
        unifiedDevice.reset();
    }

    if (entries.empty())
    {
        syslog (LOG_INFO, "GLA: empty map, no device created");
        return;
    }

    syslog (LOG_INFO, "GLA: creating GLAUnifiedDevice");
    unifiedDevice = std::make_shared<GLAUnifiedDevice> (context, entries);
    syslog (LOG_INFO, "GLA: calling init()");
    unifiedDevice->init();
    syslog (LOG_INFO, "GLA: calling AddDevice");
    plugin->AddDevice (unifiedDevice);
    syslog (LOG_INFO, "GLA: AddDevice done");

    for (const auto& e : entries)
        usbReader->setChannelBuffer (e.channelIndex,
                                     unifiedDevice->getChannelRingBuffer (e.channelIndex));

    syslog (LOG_INFO, "GLA: applied channel map (%zu sources)", entries.size());
}
