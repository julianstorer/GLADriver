#pragma once

#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <poll.h>
#include <syslog.h>
#include <vector>
#include "../common/GLA_IPCTypes.h"
#include "../common/GLA_Socket.h"
#include "GLA_Log.h"
#include "GLA_UnifiedDevice.h"
#include "GLA_IPCClient.h"


//==============================================================================
struct GLADriver  : public aspl::Driver
{
    GLADriver() : aspl::Driver(),
                  ipcClient (std::make_shared<GLAIPCClient>())
    {}

    ~GLADriver() override
    {
        ipcClient->stop();
    }

    OSStatus Initialize() override
    {
        glaLog (LOG_INFO, "GLA: Initialize() start");

        OSStatus err = aspl::Driver::Initialize();

        if (err != noErr)
        {
            glaLog (LOG_ERR, "GLA: aspl::Driver::Initialize() failed: %d", (int) err);
            return err;
        }

        // Restore the last-known map from storage, or fetch it synchronously
        // from the app if storage is empty (first run). CoreAudio rejects a
        // plugin that registers no device before Initialize() returns.
        auto map = loadSavedChannelMap();

        if (map.empty())
        {
            glaLog (LOG_INFO, "GLA: no saved map, attempting sync fetch from app");
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
            glaLog (LOG_INFO, "GLA: no channel map available, registering stub device");
        }

        glaLog (LOG_INFO, "GLA: applying initial map (%zu channels)", map.size());
        applyChannelMap (map);

        ipcClient->start (
            [this] (const std::vector<GLAChannelEntry>& entries)
            {
                applyChannelMap (entries);
                saveChannelMap (entries);
            },
            {},  // bridge UID no longer used by driver; app handles USB capture
            [this] (uint32_t channelCount, uint32_t frameCount,
                    double sourceRate, const float* interleaved)
            {
                handleAudioData (channelCount, frameCount, sourceRate, interleaved);
            }
        );

        glaLog (LOG_INFO, "GLA: driver initialized");
        return noErr;
    }

    void applyChannelMap (const std::vector<GLAChannelEntry>& entries)
    {
        glaLog (LOG_INFO, "GLA: applyChannelMap(%zu entries)", entries.size());

        auto plugin = GetPlugin();

        // --- Empty map: remove device entirely ---
        if (entries.empty())
        {
            if (unifiedDevice)
            {
                glaLog (LOG_INFO, "GLA: empty map, removing device");
                plugin->RemoveDevice (unifiedDevice);
                {
                    std::lock_guard<std::mutex> lk (fifoMutex_);
                    channelFifos_.clear();
                }
                unifiedDevice.reset();
            }
            else
            {
                glaLog (LOG_INFO, "GLA: empty map, no device to remove");
            }

            return;
        }

        const bool isStub = (entries.size() == 1 && entries[0].entityId == 0);

        // --- No existing device: create and register fresh ---
        if (! unifiedDevice)
        {
            glaLog (LOG_INFO, "GLA: creating GLAUnifiedDevice (%zu channels)", entries.size());
            unifiedDevice = std::make_shared<GLAUnifiedDevice> (GetContext(), entries);
            if (! isStub) unifiedDevice->init();
            plugin->AddDevice (unifiedDevice);
            glaLog (LOG_INFO, "GLA: AddDevice done");

            if (! isStub)
            {
                std::lock_guard<std::mutex> lk (fifoMutex_);
                channelFifos_.clear();

                for (const auto& e : entries)
                {
                    const uint32_t ch = static_cast<uint32_t> (e.channelIndex);

                    if (ch >= channelFifos_.size())
                        channelFifos_.resize (ch + 1, nullptr);

                    channelFifos_[ch] = unifiedDevice->getChannelFIFO (e.channelIndex);
                }
            }

            glaLog (LOG_INFO, "GLA: applied channel map (%zu sources)", entries.size());
            return;
        }

        // --- Existing device, going to stub: full replace ---
        if (isStub)
        {
            glaLog (LOG_INFO, "GLA: replacing with stub device");
            plugin->RemoveDevice (unifiedDevice);
            {
                std::lock_guard<std::mutex> lk (fifoMutex_);
                channelFifos_.clear();
            }
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
        glaLog (LOG_INFO, "GLA: reconfiguring unified device in-place (%zu channels)", entries.size());

        const uint32_t thisGen = ++reconfigGeneration;

        // Null out FIFO pointers before the reconfigure starts. updateChannelMap
        // destroys the old rings before calling onRingsReady — without this clear,
        // handleAudioData could dereference a dangling pointer in that window.
        {
            std::lock_guard<std::mutex> lk (fifoMutex_);
            channelFifos_.clear();
        }

        unifiedDevice->updateChannelMap (entries, [this, entries, thisGen]()
        {
            // If a newer applyChannelMap has already been queued, skip.
            if (thisGen != reconfigGeneration.load (std::memory_order_relaxed))
            {
                glaLog (LOG_INFO, "GLA: skipping stale reconfigure (gen %u, now %u)",
                        thisGen, reconfigGeneration.load());
                return;
            }

            // Runs on HAL non-realtime thread inside PerformConfigurationChange.
            // New rings are live; update the FIFO pointer table.
            {
                std::lock_guard<std::mutex> lk (fifoMutex_);
                channelFifos_.clear();

                for (const auto& e : entries)
                {
                    const uint32_t ch = static_cast<uint32_t> (e.channelIndex);

                    if (ch >= channelFifos_.size())
                        channelFifos_.resize (ch + 1, nullptr);

                    channelFifos_[ch] = unifiedDevice->getChannelFIFO (e.channelIndex);
                }
            }

            glaLog (LOG_INFO, "GLA: in-place reconfigure complete (%zu sources)", entries.size());
        });
    }

private:
    //==============================================================================
    void handleAudioData (uint32_t channelCount, uint32_t frameCount,
                          double sourceRate, const float* interleaved)
    {
        const bool doLog = ((++audioDataCallCount_ % 500) == 0);

        // Compute peak for diagnostics.
        float peak = 0.0f;

        if (doLog)
        {
            const uint32_t total = channelCount * frameCount;

            for (uint32_t i = 0; i < total; ++i)
            {
                const float v = interleaved[i] < 0 ? -interleaved[i] : interleaved[i];
                if (v > peak) peak = v;
            }
        }

        {
            std::lock_guard<std::mutex> lk (fifoMutex_);

            // Only call setSourceRate when the rate actually changes — it calls
            // ring_.reset() which wipes the buffer, so calling it every frame
            // guarantees the ring is always empty when the HAL reads it.
            if (sourceRate != lastAudioSourceRate_)
            {
                lastAudioSourceRate_ = sourceRate;

                for (auto* fifo : channelFifos_)
                    if (fifo) fifo->setSourceRate (sourceRate);
            }

            if (audioScratch_.size() < frameCount)
                audioScratch_.resize (frameCount);

            for (uint32_t ch = 0; ch < channelCount; ++ch)
            {
                if (ch >= channelFifos_.size() || ! channelFifos_[ch])
                    continue;

                for (uint32_t f = 0; f < frameCount; ++f)
                    audioScratch_[f] = interleaved[f * channelCount + ch];

                channelFifos_[ch]->write (audioScratch_.data(), frameCount);
            }
        }

        if (doLog)
            glaLog (LOG_INFO,
                    "GLA: AudioData #%llu  ch=%u  frames=%u  peak=%.6f",
                    (unsigned long long) audioDataCallCount_, channelCount, frameCount, peak);
    }

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
        glaLog (LOG_INFO, "GLA: loaded %u channels from storage", count);
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
                        glaLog (LOG_INFO, "GLA: sync fetch got %u channels", count);
                    }
                }
            }
        }

        close (fd);
        return result;
    }

    //==============================================================================
    std::atomic<uint32_t>        reconfigGeneration { 0 };
    std::shared_ptr<GLAIPCClient> ipcClient;
    std::shared_ptr<GLAUnifiedDevice> unifiedDevice;

    // Audio data path: written by IPC thread, pointers updated by HAL non-realtime thread.
    std::mutex                     fifoMutex_;
    std::vector<GLAResamplingFIFO*> channelFifos_;
    std::vector<float>             audioScratch_;

    uint64_t audioDataCallCount_  = 0;
    double   lastAudioSourceRate_ = 0.0;
};
