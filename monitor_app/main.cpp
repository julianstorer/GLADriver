#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <signal.h>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

static constexpr const char* kGLADeviceUID = "com.greenlight.gla-injector.unified";
static constexpr int kMaxChannels = 64;
static constexpr int kBarWidth    = 20;

static std::atomic<float> gPeaks[kMaxChannels];
static std::atomic<bool>  gLayoutChanged { false };
static std::atomic<bool>  gDeviceChanged { false };
static std::atomic<bool>  gQuit          { false };

static void onSignal(int)
{
    // Restore cursor immediately so it's recovered even if cleanup hangs.
    printf ("\033[?25h");
    fflush (stdout);
    gQuit.store(true);
}

//==============================================================================
static std::string getStringProp (AudioDeviceID id,
                                   AudioObjectPropertySelector sel,
                                   AudioObjectPropertyScope scope   = kAudioObjectPropertyScopeGlobal,
                                   AudioObjectPropertyElement elem  = kAudioObjectPropertyElementMain)
{
    AudioObjectPropertyAddress prop { sel, scope, elem };
    CFStringRef cfStr = nullptr;
    UInt32 sz = sizeof (CFStringRef);
    if (AudioObjectGetPropertyData (id, &prop, 0, nullptr, &sz, &cfStr) != noErr || !cfStr)
        return {};
    char buf[256] = {};
    CFStringGetCString (cfStr, buf, sizeof (buf), kCFStringEncodingUTF8);
    CFRelease (cfStr);
    return buf;
}

static int getInputChannelCount (AudioDeviceID id)
{
    AudioObjectPropertyAddress prop { kAudioDevicePropertyStreamConfiguration,
                                      kAudioObjectPropertyScopeInput,
                                      kAudioObjectPropertyElementMain };
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize (id, &prop, 0, nullptr, &dataSize) != noErr)
        return 0;
    std::vector<uint8_t> buf (dataSize);
    if (AudioObjectGetPropertyData (id, &prop, 0, nullptr, &dataSize, buf.data()) != noErr)
        return 0;
    const auto* list = reinterpret_cast<const AudioBufferList*> (buf.data());
    int total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        total += static_cast<int> (list->mBuffers[i].mNumberChannels);
    return total;
}

static AudioDeviceID findGLADevice()
{
    AudioObjectPropertyAddress prop { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize) != noErr)
        return kAudioDeviceUnknown;
    std::vector<AudioDeviceID> ids (dataSize / sizeof (AudioDeviceID));
    AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, ids.data());
    for (auto devId : ids)
        if (getStringProp (devId, kAudioDevicePropertyDeviceUID) == kGLADeviceUID)
            return devId;
    return kAudioDeviceUnknown;
}

static std::vector<std::string> getChannelNames (AudioDeviceID id, int count)
{
    std::vector<std::string> names (static_cast<size_t> (count));
    for (int ch = 1; ch <= count; ++ch)
    {
        auto name = getStringProp (id, kAudioObjectPropertyElementName,
                                   kAudioObjectPropertyScopeInput,
                                   static_cast<AudioObjectPropertyElement> (ch));
        names[static_cast<size_t> (ch - 1)] = name.empty() ? ("Ch " + std::to_string (ch)) : name;
    }
    return names;
}

//==============================================================================
static OSStatus ioProcCallback (AudioDeviceID, const AudioTimeStamp*,
                                 const AudioBufferList* inData, const AudioTimeStamp*,
                                 AudioBufferList*, const AudioTimeStamp*, void*)
{
    int globalCh = 0;
    for (UInt32 b = 0; b < inData->mNumberBuffers && globalCh < kMaxChannels; ++b)
    {
        const auto& abuf   = inData->mBuffers[b];
        const auto  nCh    = static_cast<int> (abuf.mNumberChannels);
        const auto  frames = abuf.mDataByteSize / (sizeof (float) * abuf.mNumberChannels);
        const float* src   = static_cast<const float*> (abuf.mData);

        for (int ch = 0; ch < nCh && globalCh < kMaxChannels; ++ch, ++globalCh)
        {
            float peak = 0.0f;
            for (UInt32 f = 0; f < frames; ++f)
            {
                float s = std::abs (src[f * static_cast<UInt32> (nCh) + static_cast<UInt32> (ch)]);
                if (s > peak) peak = s;
            }
            gPeaks[globalCh].store (peak, std::memory_order_relaxed);
        }
    }
    return noErr;
}

static OSStatus streamConfigChanged (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*)
{
    gLayoutChanged.store (true, std::memory_order_relaxed);
    return noErr;
}

static OSStatus deviceListChanged (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*)
{
    gDeviceChanged.store (true, std::memory_order_relaxed);
    return noErr;
}

//==============================================================================
static void drawMeter (float peak)
{
    const float db      = (peak > 0.0f) ? 20.0f * std::log10 (peak) : -61.0f;
    const float clamped = std::max (-60.0f, std::min (0.0f, db));
    const int   filled  = static_cast<int> ((clamped + 60.0f) / 60.0f * kBarWidth);

    putchar ('[');
    for (int i = 0; i < kBarWidth; ++i)
        fputs (i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91", stdout);  // █ vs ░
    putchar (']');

    if (db < -60.0f)
        printf ("  -inf dBFS");
    else
        printf ("  %+6.1f dBFS", db);
}

//==============================================================================
int main()
{
    // SA_RESETHAND: first Ctrl-C sets gQuit and lets cleanup run;
    // if cleanup hangs, a second Ctrl-C uses the default handler (immediate kill).
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sa.sa_flags   = SA_RESETHAND;
    sigaction (SIGINT,  &sa, nullptr);
    sigaction (SIGTERM, &sa, nullptr);

    printf ("\033[?25l");   // hide cursor
    fflush (stdout);

    AudioObjectPropertyAddress devListProp { kAudioHardwarePropertyDevices,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener (kAudioObjectSystemObject, &devListProp, deviceListChanged, nullptr);

    AudioDeviceID            deviceId     = kAudioDeviceUnknown;
    AudioDeviceIOProcID      ioProcId     = nullptr;
    int                      channelCount = 0;
    std::vector<std::string> channelNames;
    float                    displayPeaks[kMaxChannels] = {};

    auto disconnect = [&]()
    {
        if (ioProcId != nullptr)
        {
            AudioDeviceStop (deviceId, ioProcId);
            AudioDeviceDestroyIOProcID (deviceId, ioProcId);
            ioProcId = nullptr;
        }
        if (deviceId != kAudioDeviceUnknown)
        {
            AudioObjectPropertyAddress cfgProp { kAudioDevicePropertyStreamConfiguration,
                                                 kAudioObjectPropertyScopeInput,
                                                 kAudioObjectPropertyElementMain };
            AudioObjectRemovePropertyListener (deviceId, &cfgProp, streamConfigChanged, nullptr);

            deviceId = kAudioDeviceUnknown;
        }
        channelCount = 0;
        channelNames.clear();
        for (auto& p : displayPeaks) p = 0.0f;
        for (int i = 0; i < kMaxChannels; ++i)
            gPeaks[i].store (0.0f, std::memory_order_relaxed);
    };

    auto connect = [&]() -> bool
    {
        deviceId = findGLADevice();
        if (deviceId == kAudioDeviceUnknown) return false;

        channelCount = getInputChannelCount (deviceId);
        if (channelCount <= 0) return false;

        channelNames = getChannelNames (deviceId, channelCount);

        AudioObjectPropertyAddress cfgProp { kAudioDevicePropertyStreamConfiguration,
                                             kAudioObjectPropertyScopeInput,
                                             kAudioObjectPropertyElementMain };

        if (AudioDeviceCreateIOProcID (deviceId, ioProcCallback, nullptr, &ioProcId) != noErr)
        {
            deviceId = kAudioDeviceUnknown;
            return false;
        }

        // Register listener only after IOProc succeeds so we never leak it.
        AudioObjectAddPropertyListener (deviceId, &cfgProp, streamConfigChanged, nullptr);

        if (AudioDeviceStart (deviceId, ioProcId) != noErr)
        {
            AudioObjectRemovePropertyListener (deviceId, &cfgProp, streamConfigChanged, nullptr);
            AudioDeviceDestroyIOProcID (deviceId, ioProcId);
            ioProcId  = nullptr;
            deviceId  = kAudioDeviceUnknown;
            return false;
        }
        return true;
    };

    printf ("\033[2J\033[H");
    printf ("GLA Monitor — waiting for '%s'...\n", kGLADeviceUID);
    fflush (stdout);

    bool connected  = false;
    int  headerRows = 0;

    while (!gQuit.load())
    {
        if (!connected)
        {
            gLayoutChanged.store (false, std::memory_order_relaxed);
            gDeviceChanged.store (false, std::memory_order_relaxed);

            if (connect())
            {
                connected   = true;
                headerRows  = 2;   // header line + blank line
                printf ("\033[2J\033[H");
                printf ("GLA Monitor — %s  |  %d channels\n\n",
                        getStringProp (deviceId, kAudioObjectPropertyName).c_str(), channelCount);
                for (int i = 0; i < channelCount; ++i)
                    putchar ('\n');
                fflush (stdout);
            }
            else
            {
                usleep (1'000'000);
                continue;
            }
        }

        if (gDeviceChanged.exchange (false, std::memory_order_relaxed))
        {
            // Always disconnect: the AudioDeviceID may have changed even if the
            // UID-based lookup still finds a device (destroy+recreate on reconfig).
            disconnect();
            connected = false;
            printf ("\033[2J\033[H");
            if (findGLADevice() == kAudioDeviceUnknown)
            {
                printf ("GLA Monitor — device removed, waiting...\n");
                fflush (stdout);
                usleep (1'000'000);
            }
            else
            {
                printf ("GLA Monitor — device changed, reconnecting...\n");
                fflush (stdout);
                usleep (200'000);
            }
            continue;
        }

        if (gLayoutChanged.exchange (false, std::memory_order_relaxed))
        {
            const int oldCount = channelCount;
            disconnect();
            connected = false;
            printf ("\033[2J\033[H");
            printf ("GLA Monitor — channel layout changed (was %d ch), reconnecting...\n", oldCount);
            fflush (stdout);
            usleep (200'000);
            continue;
        }

        // Re-read names every frame — cheap CFString calls, catches any rename
        // regardless of what property notifications the HAL did or didn't send.
        channelNames = getChannelNames (deviceId, channelCount);

        // Redraw all meter rows in-place
        printf ("\033[%d;1H", headerRows + 1);

        for (int ch = 0; ch < channelCount; ++ch)
        {
            const float raw  = gPeaks[ch].load (std::memory_order_relaxed);
            displayPeaks[ch] = std::max (displayPeaks[ch] * 0.92f, raw);

            printf ("CH %02d  %-20.20s  ", ch + 1,
                    channelNames[static_cast<size_t> (ch)].c_str());
            drawMeter (displayPeaks[ch]);
            printf ("\033[K\n");
        }
        fflush (stdout);

        usleep (50'000);   // ~20 fps
    }

    disconnect();
    AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &devListProp, deviceListChanged, nullptr);

    printf ("\033[2J\033[H");
    printf ("\033[?25h");   // restore cursor
    printf ("GLA Monitor — exited.\n");
    fflush (stdout);

    return 0;
}
