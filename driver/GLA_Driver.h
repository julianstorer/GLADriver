#pragma once

#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <memory>
#include <syslog.h>
#include <vector>
#include "../common/GLA_IPCTypes.h"
#include "GLA_UnifiedDevice.h"
#include "GLA_IPCClient.h"
#include "GLA_USBReader.h"


//==============================================================================
struct GLADriver  : public aspl::Driver
{
    GLADriver() : aspl::Driver(),
                  usbReader (std::make_shared<GLAUSBReader>()),
                  ipcClient (std::make_shared<GLAIPCClient>())
    {}

    ~GLADriver() override
    {
        ipcClient->stop();
        usbReader->stop();
    }

    OSStatus Initialize() override
    {
        syslog (LOG_INFO, "GLA: Initialize() start");

        OSStatus err = aspl::Driver::Initialize();

        if (err != noErr)
        {
            syslog (LOG_ERR, "GLA: aspl::Driver::Initialize() failed: %d", (int) err);
            return err;
        }

        const auto& map = testChannelMap();
        syslog (LOG_INFO, "GLA: calling applyChannelMap (test map, %zu entries)", map.size());
        applyChannelMap (map);

        ipcClient->start (
            [this] (const std::vector<GLAChannelEntry>& entries) { applyChannelMap (entries); },
            [this] (const std::string& uid)                      { applyUSBBridge (uid); }
        );

        syslog (LOG_INFO, "GLA: driver initialized");
        return noErr;
    }

    void applyUSBBridge (const std::string& uid)
    {
        syslog (LOG_INFO, "GLA: applyUSBBridge('%s')", uid.c_str());
        currentBridgeUID = uid;

        if (unifiedDevice)
        {
            usbReader->stop();
            usbReader->start (currentBridgeUID);
        }
    }

    void applyChannelMap (const std::vector<GLAChannelEntry>& entries)
    {
        syslog (LOG_INFO, "GLA: applyChannelMap(%zu entries)", entries.size());

        auto plugin = GetPlugin();

        if (unifiedDevice)
        {
            syslog (LOG_INFO, "GLA: removing old unified device");
            usbReader->stop();
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
        unifiedDevice = std::make_shared<GLAUnifiedDevice> (GetContext(), entries);
        syslog (LOG_INFO, "GLA: calling init()");
        unifiedDevice->init();
        syslog (LOG_INFO, "GLA: calling AddDevice");
        plugin->AddDevice (unifiedDevice);
        syslog (LOG_INFO, "GLA: AddDevice done");

        for (const auto& e : entries)
            usbReader->setChannelBuffer (e.channelIndex,
                                         unifiedDevice->getChannelRingBuffer (e.channelIndex));

        if (! currentBridgeUID.empty())
        {
            syslog (LOG_INFO, "GLA: starting USB reader for '%s'", currentBridgeUID.c_str());
            usbReader->start (currentBridgeUID);
        }

        syslog (LOG_INFO, "GLA: applied channel map (%zu sources)", entries.size());
    }

private:
    //==============================================================================
    static const std::vector<GLAChannelEntry>& testChannelMap()
    {
        static const std::vector<GLAChannelEntry> map = []()
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

        return map;
    }

    std::string currentBridgeUID;
    std::shared_ptr<GLAUSBReader> usbReader;
    std::shared_ptr<GLAIPCClient> ipcClient;
    std::shared_ptr<GLAUnifiedDevice> unifiedDevice;
};
