#pragma once

#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <dispatch/dispatch.h>
#include <memory>
#include <poll.h>
#include <syslog.h>
#include <vector>
#include "../common/GLA_IPCTypes.h"
#include "../common/GLA_Socket.h"
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

        // Restore the last-known map from storage, or fetch it synchronously
        // from the app if storage is empty (first run). CoreAudio rejects a
        // plugin that registers no device before Initialize() returns.
        auto map = loadSavedChannelMap();

        if (map.empty())
        {
            syslog (LOG_INFO, "GLA: no saved map, attempting sync fetch from app");
            map = syncFetchChannelMap();
        }

        if (map.empty())
        {
            // CoreAudio rejects a plugin that registers no device before
            // Initialize() returns. Use a 1-channel stub so the driver stays
            // loaded; the IPC thread will replace it once the app sends a real map.
            GLAChannelEntry stub {};
            stub.channelIndex = 0;
            stub.entityId     = 0;
            strncpy (stub.displayName, "GLA (unconfigured)", sizeof (stub.displayName) - 1);
            map.push_back (stub);
            syslog (LOG_INFO, "GLA: no channel map available, registering stub device");
        }

        syslog (LOG_INFO, "GLA: applying initial map (%zu channels)", map.size());
        applyChannelMap (map);

        ipcClient->start (
            [this] (const std::vector<GLAChannelEntry>& entries)
            {
                applyChannelMap (entries);
                saveChannelMap (entries);
            },
            [this] (const std::string& uid) { applyUSBBridge (uid); }
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

        // --- Empty map: remove device entirely ---
        if (entries.empty())
        {
            usbReader->stop();

            if (unifiedDevice)
            {
                syslog (LOG_INFO, "GLA: empty map, removing device");
                plugin->RemoveDevice (unifiedDevice);
                usbReader->clearChannelBuffers();
                unifiedDevice.reset();
            }
            else
            {
                syslog (LOG_INFO, "GLA: empty map, no device to remove");
            }

            return;
        }

        const bool isStub = (entries.size() == 1 && entries[0].entityId == 0);

        // --- No existing device: create and register fresh ---
        if (! unifiedDevice)
        {
            syslog (LOG_INFO, "GLA: creating GLAUnifiedDevice (%zu channels)", entries.size());
            unifiedDevice = std::make_shared<GLAUnifiedDevice> (GetContext(), entries);
            if (! isStub) unifiedDevice->init();
            plugin->AddDevice (unifiedDevice);
            syslog (LOG_INFO, "GLA: AddDevice done");

            if (! isStub)
            {
                for (const auto& e : entries)
                    usbReader->setChannelBuffer (e.channelIndex,
                                                 unifiedDevice->getChannelFIFO (e.channelIndex));

                if (! currentBridgeUID.empty())
                {
                    syslog (LOG_INFO, "GLA: starting USB reader for '%s'", currentBridgeUID.c_str());
                    usbReader->start (currentBridgeUID);
                }
            }

            syslog (LOG_INFO, "GLA: applied channel map (%zu sources)", entries.size());
            return;
        }

        // --- Existing device, going to stub: full replace ---
        // Stubs have no stream and no IO handler; in-place reconfiguration would
        // require special-casing. Just do a clean remove+add. The stub appears
        // briefly, so the timing gap is harmless — no client should be streaming.
        if (isStub)
        {
            syslog (LOG_INFO, "GLA: replacing with stub device");
            usbReader->stop();
            plugin->RemoveDevice (unifiedDevice);
            usbReader->clearChannelBuffers();
            unifiedDevice.reset();

            unifiedDevice = std::make_shared<GLAUnifiedDevice> (GetContext(), entries);
            plugin->AddDevice (unifiedDevice);
            return;
        }

        // --- Existing device, normal non-stub update: reconfigure in-place ---
        //
        // We MUST NOT do RemoveDevice + AddDevice here. Both operations share the
        // same DeviceUID string. If coreaudiod processes the PropertiesChanged
        // notifications out of order, or a client queries between Remove and Add,
        // it can get a stale UID→ID mapping and hang on any subsequent CoreAudio
        // call against the GLA device — including AudioDeviceStart in the monitor.
        //
        // RequestConfigurationChange tells coreaudiod "reconfigure this device
        // when you're ready". coreaudiod quiesces IO, calls PerformConfigurationChange,
        // the lambda runs (stream swap + ring-buffer replacement), then IO resumes.
        // The AudioDeviceID stays constant; clients see kAudioDevicePropertyStreamConfiguration
        // changed and reconnect gracefully.
        syslog (LOG_INFO, "GLA: reconfiguring unified device in-place (%zu channels)", entries.size());
        usbReader->stop();

        std::string uid = currentBridgeUID;

        unifiedDevice->updateChannelMap (entries, [this, entries, uid]()
        {
            // Runs on HAL non-realtime thread inside PerformConfigurationChange.
            // New rings are live; point the USB reader at them.
            usbReader->clearChannelBuffers();

            for (const auto& e : entries)
                usbReader->setChannelBuffer (e.channelIndex,
                                             unifiedDevice->getChannelFIFO (e.channelIndex));

            if (! uid.empty())
            {
                // Start the USB reader on a separate thread — calling AudioDeviceStart
                // from inside PerformConfigurationChange is potentially re-entrant
                // into coreaudiod and must be avoided.
                dispatch_async (dispatch_get_global_queue (QOS_CLASS_USER_INITIATED, 0), ^{
                    syslog (LOG_INFO, "GLA: starting USB reader for '%s' after reconfigure", uid.c_str());
                    usbReader->start (uid);
                });
            }

            syslog (LOG_INFO, "GLA: in-place reconfigure complete (%zu sources)", entries.size());
        });
    }

private:
    //==============================================================================
    std::vector<GLAChannelEntry> loadSavedChannelMap()
    {
        auto [bytes, ok] = GetStorage()->ReadBytes ("channelMap");

        if (! ok || bytes.size() < 4)
            return {};

        uint32_t count = 0;
        memcpy (&count, bytes.data(), 4);

        if (bytes.size() < 4 + count * sizeof (GLAChannelEntry) || count == 0)
            return {};

        std::vector<GLAChannelEntry> entries (count);
        memcpy (entries.data(), bytes.data() + 4, count * sizeof (GLAChannelEntry));
        syslog (LOG_INFO, "GLA: loaded %u channels from storage", count);
        return entries;
    }

    void saveChannelMap (const std::vector<GLAChannelEntry>& entries)
    {
        if (entries.empty())
            return;

        const uint32_t count = static_cast<uint32_t> (entries.size());
        std::vector<UInt8> bytes (4 + count * sizeof (GLAChannelEntry));
        memcpy (bytes.data(),     &count,           4);
        memcpy (bytes.data() + 4, entries.data(),   count * sizeof (GLAChannelEntry));
        GetStorage()->WriteBytes ("channelMap", std::move (bytes));
    }

    // One-shot synchronous connect — used only during Initialize() when storage
    // is empty (first run). Waits up to 500ms for the app to send a channel map.
    std::vector<GLAChannelEntry> syncFetchChannelMap()
    {
        int fd = glaConnect (glaSocketPath);
        if (fd < 0)
            return {};

        struct pollfd pfd { fd, POLLIN, 0 };

        std::vector<GLAChannelEntry> result;

        if (poll (&pfd, 1, 500) > 0 && (pfd.revents & POLLIN))
        {
            std::vector<uint8_t> msg;

            if (glaRecvMessage (fd, msg) && msg.size() >= 8)
            {
                uint32_t type = 0;
                memcpy (&type, msg.data(), 4);

                if (static_cast<GLAMsgType> (type) == GLAMsgType::ChannelMapUpdate)
                {
                    uint32_t count = 0;
                    memcpy (&count, msg.data() + 4, 4);

                    if (count > 0 && msg.size() >= 8 + count * sizeof (GLAChannelEntry))
                    {
                        result.resize (count);
                        memcpy (result.data(), msg.data() + 8, count * sizeof (GLAChannelEntry));
                        syslog (LOG_INFO, "GLA: sync fetch got %u channels", count);
                    }
                }
            }
        }

        close (fd);
        return result;
    }

    //==============================================================================
    std::string currentBridgeUID;
    std::shared_ptr<GLAUSBReader> usbReader;
    std::shared_ptr<GLAIPCClient> ipcClient;
    std::shared_ptr<GLAUnifiedDevice> unifiedDevice;
};
