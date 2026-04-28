
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <vector>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>
#include "../../common/GLA_IPCTypes.h"
#include "GLA_AppBackend.h"


//==============================================================================
// Main UI component.
// Shows: Network Interface | USB Bridge | Entity-to-channel patchbay | Status label.
class MainComponent : public juce::Component,
                      public juce::ComboBox::Listener,
                      public juce::Timer
{
public:
    MainComponent()
    {
        setSize (600, 500);

        addAndMakeVisible (labelNetif);
        addAndMakeVisible (comboNetif);
        addAndMakeVisible (labelBridge);
        addAndMakeVisible (comboBridge);
        addAndMakeVisible (labelStatus);
        addAndMakeVisible (labelDriver);

        comboNetif.addListener (this);
        comboBridge.addListener (this);

        refreshNetworkInterfaces();

        backend.setEntityListCallback ([this] (const std::vector<GLAEntityInfo>& e)
        {
            onEntityListReceived (e);
        });

        backend.setChannelMapCallback ([this] (const std::vector<GLAChannelEntry>& m)
        {
            onChannelMapReceived (m);
        });

        std::string startIface = netifBSDNames.empty() ? "en0" : netifBSDNames.front();
        if (!backend.start (startIface))
            labelStatus.setText ("Status: IPC server failed to start", juce::dontSendNotification);

        startTimer (5000);
    }

    ~MainComponent() override
    {
        stopTimer();
        backend.stop();
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
        g.setColour (juce::Colours::white);
        g.setFont (14.0f);
        g.drawText ("GLA Injector - Patchbay", 10, 10, getWidth() - 20, 24, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        area.removeFromTop (30); // title

        labelNetif.setBounds (area.removeFromTop (20));
        comboNetif.setBounds (area.removeFromTop (rowHeight));
        area.removeFromTop (4);
        labelBridge.setBounds (area.removeFromTop (20));
        comboBridge.setBounds (area.removeFromTop (rowHeight));
        area.removeFromTop (8);
        labelStatus.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        labelDriver.setBounds (area.removeFromTop (20));
        area.removeFromTop (8);

        for (auto& row : patchRows)
        {
            auto rowArea = area.removeFromTop (rowHeight);
            row.label->setBounds (rowArea.removeFromLeft (60));
            row.combo->setBounds (rowArea);
            area.removeFromTop (2);
        }
    }

    void comboBoxChanged (juce::ComboBox* box) override
    {
        if (box == &comboNetif)
        {
            int idx = comboNetif.getSelectedId() - 1;
            if (idx >= 0 && idx < static_cast<int> (netifBSDNames.size()))
                backend.setNetworkInterface (netifBSDNames[static_cast<size_t> (idx)]);
        }
        else if (box == &comboBridge)
        {
            int idx = comboBridge.getSelectedId() - 1;
            if (idx >= 0 && idx < static_cast<int> (bridgeUIDs.size()))
                backend.setUSBBridge (bridgeUIDs[static_cast<size_t> (idx)]);
        }
        else
        {
            // It's a patchbay row combo.
            for (int ch = 0; ch < static_cast<int> (patchRows.size()); ++ch)
            {
                if (patchRows[static_cast<size_t> (ch)].combo.get() == box)
                {
                    // usbChannel is the actual channelIndex this row represents,
                    // kept in sync by syncCombosToMap().
                    const int usbCh   = patchRows[static_cast<size_t> (ch)].usbChannel;
                    const int selId   = box->getSelectedId();

                    if (selId == 1000)
                    {
                        backend.setRouting (static_cast<uint8_t> (usbCh), 0);
                    }
                    else if (selId > 0 &&
                             static_cast<size_t> (selId - 1) < entities.size())
                    {
                        auto eid = entities[static_cast<size_t> (selId - 1)].entityId;
                        backend.setRouting (static_cast<uint8_t> (usbCh), eid);
                    }
                    break;
                }
            }
        }
    }

    void timerCallback() override
    {
        refreshNetworkInterfaces();
        auto devices = getAudioInputDevices();
        int current = comboBridge.getSelectedId();
        comboBridge.clear (juce::dontSendNotification);
        bridgeUIDs.clear();

        for (int i = 0; i < static_cast<int> (devices.size()); ++i)
        {
            bridgeUIDs.push_back (devices[static_cast<size_t> (i)].second);
            comboBridge.addItem (devices[static_cast<size_t> (i)].first, i + 1);
        }

        comboBridge.setSelectedId (current, juce::dontSendNotification);

        labelDriver.setText (backend.getDriverStatus(), juce::dontSendNotification);
    }


    static constexpr int maxBridgeChannels = 32;
    static constexpr int rowHeight         = 28;

private:
    //==============================================================================
    void refreshNetworkInterfaces()
    {
        auto ifaces = getNetworkInterfaces();
        int current = comboNetif.getSelectedId();
        comboNetif.clear (juce::dontSendNotification);
        netifBSDNames.clear();

        for (int i = 0; i < static_cast<int> (ifaces.size()); ++i)
        {
            auto& [disp, bsd] = ifaces[static_cast<size_t> (i)];
            netifBSDNames.push_back (bsd);
            comboNetif.addItem (juce::String (disp) + " (" + juce::String (bsd) + ")", i + 1);
        }

        comboNetif.setSelectedId (current > 0 ? current : 1, juce::dontSendNotification);
    }

    void onEntityListReceived (const std::vector<GLAEntityInfo>& newEntities)
    {
        entities = newEntities;
        labelStatus.setText (juce::String (static_cast<int> (newEntities.size())) +
                            " entities on network", juce::dontSendNotification);
        rebuildPatchbay();
    }

    void onChannelMapReceived (const std::vector<GLAChannelEntry>& entries)
    {
        channelMap = entries;
        syncCombosToMap();
    }

    void syncCombosToMap()
    {
        // Reset all rows to "(none)"
        for (auto& row : patchRows)
            row.combo->setSelectedId (1000, juce::dontSendNotification);

        // Sort active entries by channelIndex so display order is stable
        auto sorted = channelMap;
        std::sort (sorted.begin(), sorted.end(), [] (const GLAChannelEntry& a, const GLAChannelEntry& b)
        {
            return a.channelIndex < b.channelIndex;
        });

        // Compute first fresh channelIndex for empty rows (one past the highest active)
        int nextFresh = 0;
        for (auto const& e : sorted)
            nextFresh = std::max (nextFresh, static_cast<int> (e.channelIndex) + 1);

        // Assign active entries to consecutive rows with no gaps
        for (size_t r = 0; r < patchRows.size(); ++r)
        {
            if (r < sorted.size())
            {
                patchRows[r].usbChannel = sorted[r].channelIndex;

                for (int i = 0; i < static_cast<int> (entities.size()); ++i)
                {
                    if (entities[static_cast<size_t> (i)].entityId == sorted[r].entityId)
                    {
                        patchRows[r].combo->setSelectedId (i + 1, juce::dontSendNotification);
                        break;
                    }
                }
            }
            else
            {
                // Empty rows get fresh channelIndex values so new selections append cleanly
                patchRows[r].usbChannel = nextFresh + static_cast<int> (r - sorted.size());
            }
        }
    }

    void rebuildPatchbay()
    {
        for (auto& row : patchRows)
        {
            removeChildComponent (row.label.get());
            removeChildComponent (row.combo.get());
        }

        patchRows.clear();

        int rowCount = (bridgeChannelCount > 0) ? bridgeChannelCount : 8;
        rowCount = std::min (rowCount, maxBridgeChannels);

        for (int ch = 0; ch < rowCount; ++ch)
        {
            PatchRow row;
            row.usbChannel = ch;
            row.label = std::make_unique<juce::Label> ("", "Ch " + juce::String (ch + 1));
            row.combo = std::make_unique<juce::ComboBox>();
            row.combo->addItem ("(none)", 1000);

            for (int i = 0; i < static_cast<int> (entities.size()); ++i)
            {
                auto& ent = entities[static_cast<size_t> (i)];
                juce::String name = ent.name;
                if (!ent.online) name = "[offline] " + name;
                row.combo->addItem (name, i + 1);
            }

            row.combo->addListener (this);
            addAndMakeVisible (*row.label);
            addAndMakeVisible (*row.combo);
            patchRows.push_back (std::move (row));
        }

        resized();
        syncCombosToMap();
    }

    //==============================================================================
    AppBackend backend;

    juce::Label    labelNetif  { "", "Network Interface:" };
    juce::ComboBox comboNetif;
    juce::Label    labelBridge { "", "USB Bridge:" };
    juce::ComboBox comboBridge;
    juce::Label    labelStatus { "", "Status: starting..." };
    juce::Label    labelDriver { "", "Driver: checking..." };

    // Patchbay: one row per USB channel slot.
    struct PatchRow
    {
        int usbChannel;
        std::unique_ptr<juce::Label>    label;
        std::unique_ptr<juce::ComboBox> combo;
    };

    std::vector<PatchRow> patchRows;
    std::vector<GLAEntityInfo> entities;
    std::vector<GLAChannelEntry> channelMap;

    int bridgeChannelCount = 0;

    std::vector<std::string> netifBSDNames;
    std::vector<std::string> bridgeUIDs;

    //==============================================================================
    // Enumerate CoreAudio input devices (potential USB bridges).
    static std::vector<std::pair<std::string, std::string>> getAudioInputDevices()
    {
        AudioObjectPropertyAddress prop =
        {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        UInt32 dataSize = 0;
        AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
        std::vector<AudioDeviceID> ids (dataSize / sizeof (AudioDeviceID));
        AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, ids.data());

        std::vector<std::pair<std::string, std::string>> result; // {name, uid}

        for (auto id : ids)
        {
            AudioObjectPropertyAddress inputProp =
            {
                kAudioDevicePropertyStreamConfiguration,
                kAudioDevicePropertyScopeInput,
                kAudioObjectPropertyElementMain
            };

            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize (id, &inputProp, 0, nullptr, &sz) != noErr)
                continue;

            std::vector<uint8_t> buf (sz);

            if (AudioObjectGetPropertyData (id, &inputProp, 0, nullptr, &sz, buf.data()) != noErr)
                continue;

            auto* list = reinterpret_cast<AudioBufferList*> (buf.data());
            int channels = 0;

            for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
                channels += static_cast<int> (list->mBuffers[b].mNumberChannels);

            if (channels < 1)
                continue;

            auto getStr = [&] (AudioObjectPropertySelector sel) -> std::string
            {
                AudioObjectPropertyAddress sp = { sel, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
                CFStringRef cf = nullptr;
                UInt32 s = sizeof (cf);

                if (AudioObjectGetPropertyData (id, &sp, 0, nullptr, &s, &cf) != noErr || !cf)
                    return {};

                char buf2[256] = {};
                CFStringGetCString (cf, buf2, sizeof (buf2), kCFStringEncodingUTF8);
                CFRelease (cf);
                return buf2;
            };

            auto uid = getStr (kAudioDevicePropertyDeviceUID);

            // Never offer our own virtual device as a bridge source — selecting it
            // would make the USB reader read back the same zeros it's supposed to fill.
            if (uid == kGLADriverUID)
                continue;

            result.push_back ({ getStr (kAudioObjectPropertyName), uid });
        }

        return result;
    }

    // Returns {displayName, bsdName} pairs for UP, non-loopback interfaces that
    // System Preferences considers real (filters out utun*, awdl*, etc.).
    static std::vector<std::pair<std::string, std::string>> getNetworkInterfaces()
    {
        // Build set of UP, non-loopback BSD names from getifaddrs.
        std::vector<std::string> upIfaces;
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs (&ifaddr) == 0)
        {
            for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
            {
                if (! ifa->ifa_addr)                  continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)    continue;
                if (! (ifa->ifa_flags & IFF_UP))      continue;
                std::string n = ifa->ifa_name;
                if (std::find (upIfaces.begin(), upIfaces.end(), n) == upIfaces.end())
                    upIfaces.push_back (n);
            }
            freeifaddrs (ifaddr);
        }

        std::vector<std::pair<std::string, std::string>> result;

        CFArrayRef scIfaces = SCNetworkInterfaceCopyAll();
        if (! scIfaces)
            return result;

        for (CFIndex i = 0; i < CFArrayGetCount (scIfaces); ++i)
        {
            auto* iface = static_cast<SCNetworkInterfaceRef> (
                CFArrayGetValueAtIndex (scIfaces, i));

            CFStringRef bsdCF = SCNetworkInterfaceGetBSDName (iface);
            if (! bsdCF) continue;

            char bsdBuf[64] = {};
            CFStringGetCString (bsdCF, bsdBuf, sizeof (bsdBuf), kCFStringEncodingUTF8);
            std::string bsd = bsdBuf;

            if (std::find (upIfaces.begin(), upIfaces.end(), bsd) == upIfaces.end())
                continue;

            std::string disp = bsd;
            CFStringRef dispCF = SCNetworkInterfaceGetLocalizedDisplayName (iface);
            if (dispCF)
            {
                char dispBuf[256] = {};
                CFStringGetCString (dispCF, dispBuf, sizeof (dispBuf), kCFStringEncodingUTF8);
                if (dispBuf[0] != '\0') disp = dispBuf;
            }

            result.push_back ({ disp, bsd });
        }

        CFRelease (scIfaces);
        return result;
    }

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
