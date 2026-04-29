#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../common/GLA_IPCTypes.h"
#include "../common/GLA_Socket.h"


struct GLAIPCClient
{
    GLAIPCClient() = default;
    ~GLAIPCClient()  { stop(); }

    using ChannelMapCallback = std::function<void (const std::vector<GLAChannelEntry>&)>;
    using BridgeCallback     = std::function<void (const std::string&)>;
    using AudioDataCallback  = std::function<void (uint32_t channelCount, uint32_t frameCount,
                                                    double sourceRate, const float* interleaved)>;

    void start (ChannelMapCallback mapCb, BridgeCallback bridgeCb = {}, AudioDataCallback audioCb = {})
    {
        callback          = std::move (mapCb);
        bridgeCallback    = std::move (bridgeCb);
        audioDataCallback = std::move (audioCb);
        running = true;
        thread = std::thread ([this] { runLoop(); });
    }

    void stop()
    {
        running = false;

        if (fd >= 0)
        {
            close (fd);
            fd = -1;
        }

        if (thread.joinable())
            thread.join();
    }

private:
    //==============================================================================
    int fd = -1;
    std::atomic<bool> running { false };
    std::thread thread;
    ChannelMapCallback callback;
    BridgeCallback     bridgeCallback;
    AudioDataCallback  audioDataCallback;

    //==============================================================================
    void runLoop()
    {
        int backoff = 1;

        while (running)
        {
            if (fd < 0)
            {
                if (! tryConnect())
                {
                    std::this_thread::sleep_for (std::chrono::seconds (backoff));
                    backoff = std::min (backoff * 2, 5);
                    continue;
                }

                backoff = 1;
                syslog (LOG_INFO, "GLA: connected to app");
            }

            std::vector<uint8_t> msg;

            if (! glaRecvMessage (fd, msg))
            {
                syslog (LOG_WARNING, "GLA: lost connection to app");
                close (fd);
                fd = -1;
                // Brief pause before reconnecting — prevents a rapid reconnect spiral
                // that floods the server with RequestConfigurationChange calls and
                // triggers fd-reuse races in the server's poll loop.
                std::this_thread::sleep_for (std::chrono::milliseconds (200));
                continue;
            }

            handleMessage (msg);
        }
    }

    bool tryConnect()
    {
        fd = glaConnect (glaSocketPath);
        return fd >= 0;
    }

    void handleMessage (const std::vector<uint8_t>& msg)
    {
        if (msg.size() < 8)
            return;

        uint32_t type = 0;
        memcpy (&type, msg.data(), 4);
        const auto msgType = static_cast<GLAMsgType> (type);

        if (msgType == GLAMsgType::ChannelMapUpdate)
        {
            uint32_t count = 0;
            memcpy (&count, msg.data() + 4, 4);

            if (msg.size() < 8 + count * sizeof (GLAChannelEntry))
                return;

            std::vector<GLAChannelEntry> entries (count);
            memcpy (entries.data(), msg.data() + 8, count * sizeof (GLAChannelEntry));

            if (callback)
                callback (entries);
        }
        else if (msgType == GLAMsgType::SetUSBBridge)
        {
            uint32_t len = 0;
            memcpy (&len, msg.data() + 4, 4);

            if (msg.size() < 8 + len)
                return;

            std::string uid (reinterpret_cast<const char*> (msg.data() + 8), len);

            if (bridgeCallback)
                bridgeCallback (uid);
        }
        else if (msgType == GLAMsgType::AudioData)
        {
            // [type:4][channelCount:4][frameCount:4][sourceRate:8][samples:4*ch*fr]
            if (msg.size() < 20)
                return;

            uint32_t channelCount = 0, frameCount = 0;
            double   sourceRate   = 48000.0;
            memcpy (&channelCount, msg.data() +  4, 4);
            memcpy (&frameCount,   msg.data() +  8, 4);
            memcpy (&sourceRate,   msg.data() + 12, 8);

            if (channelCount == 0 || channelCount > 512 ||
                frameCount   == 0 || frameCount   > 8192)
                return;

            const uint64_t expectedBytes = static_cast<uint64_t> (channelCount) * frameCount * sizeof (float);

            if (msg.size() < 20 + expectedBytes)
                return;

            if (audioDataCallback)
                audioDataCallback (channelCount, frameCount, sourceRate,
                                   reinterpret_cast<const float*> (msg.data() + 20));
        }
    }
};
