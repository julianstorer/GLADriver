
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <tuple>
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
        addAndMakeVisible (labelListener);
        addAndMakeVisible (comboListener);
        addAndMakeVisible (labelStatus);
        addAndMakeVisible (labelDriver);

        comboNetif.addListener (this);
        comboBridge.addListener (this);
        comboListener.addListener (this);

        refreshNetworkInterfaces();

        backend.setEntityListCallback ([this] (const std::vector<GLAEntityInfo>& e)
        {
            onEntityListReceived (e);
        });

        backend.setChannelMapCallback ([this] (const std::vector<AppBackend::SlotConfig>& m)
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
        area.removeFromTop (4);
        labelListener.setBounds (area.removeFromTop (20));
        comboListener.setBounds (area.removeFromTop (rowHeight));
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
            {
                bridgeChannelCount = bridgeChannelCounts[static_cast<size_t> (idx)];
                backend.setUSBBridge (bridgeUIDs[static_cast<size_t> (idx)],
                                      bridgeNames[static_cast<size_t> (idx)]);
                refreshBridgeStreamInfo();
                rebuildPatchbay();
            }
        }
        else if (box == &comboListener)
        {
            int idx = comboListener.getSelectedId() - 1;
            if (idx >= 0 && idx < static_cast<int> (listenerEntityIds.size()))
                bridgeListenerEntityId = listenerEntityIds[static_cast<size_t> (idx)];
            else
                bridgeListenerEntityId = 0;
            refreshBridgeStreamInfo();
            rebuildPatchbay();
        }
        else
        {
            // It's a patchbay row combo. Item IDs encode USB channel: id = usbCh + 1.
            for (int row = 0; row < static_cast<int> (patchRows.size()); ++row)
            {
                auto& patchRow = patchRows[static_cast<size_t> (row)];
                if (patchRow.combo.get() != box) continue;

                const int selId = box->getSelectedId();

                if (selId == 1000) // "(none)" — unassign this virtual output slot
                {
                    backend.clearSlot (row);
                    patchRow.usbChannel = -1;
                }
                else if (selId > 0) // USB channel selected; id = usbCh + 1
                {
                    int newUsbCh = selId - 1;
                    patchRow.usbChannel = newUsbCh;

                    std::string sourceName;
                    uint64_t    talkerEid = 0;
                    if (newUsbCh < static_cast<int> (usbChannelInfos.size()))
                    {
                        sourceName = usbChannelInfos[static_cast<size_t> (newUsbCh)].sourceName;
                        talkerEid  = usbChannelInfos[static_cast<size_t> (newUsbCh)].talkerEntityId;
                    }

                    backend.setSlot (row, static_cast<uint8_t> (newUsbCh), talkerEid, sourceName);
                }
                break;
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
        bridgeNames.clear();
        bridgeChannelCounts.clear();

        for (int i = 0; i < static_cast<int> (devices.size()); ++i)
        {
            bridgeNames.push_back (std::get<0> (devices[static_cast<size_t> (i)]));
            bridgeUIDs.push_back (std::get<1> (devices[static_cast<size_t> (i)]));
            bridgeChannelCounts.push_back (std::get<2> (devices[static_cast<size_t> (i)]));
            comboBridge.addItem (std::get<0> (devices[static_cast<size_t> (i)]), i + 1);
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

    void refreshBridgeStreamInfo()
    {
        usbChannelInfos = backend.getUSBChannelInfos (bridgeChannelCount, bridgeListenerEntityId);
    }

    void onEntityListReceived (const std::vector<GLAEntityInfo>& newEntities)
    {
        entities = newEntities;
        labelStatus.setText (juce::String (static_cast<int> (newEntities.size())) +
                            " entities on network", juce::dontSendNotification);

        // Rebuild listener combo, preserving the current selection by entity ID.
        int prevSel = comboListener.getSelectedId();
        comboListener.clear (juce::dontSendNotification);
        listenerEntityIds.clear();
        comboListener.addItem ("(none)", 1);
        listenerEntityIds.push_back (0);
        for (int i = 0; i < static_cast<int> (newEntities.size()); ++i)
        {
            comboListener.addItem (juce::String (newEntities[static_cast<size_t> (i)].name), i + 2);
            listenerEntityIds.push_back (newEntities[static_cast<size_t> (i)].entityId);
        }

        // Restore selection if the same entity is still present
        if (prevSel > 1)
            comboListener.setSelectedId (prevSel, juce::dontSendNotification);
        else
            comboListener.setSelectedId (1, juce::dontSendNotification);

        // Only rebuild patchbay (which sends a channel map to the driver) if the
        // listener's stream topology actually changed. Entity online/offline events
        // that don't affect the selected listener must not trigger RequestConfigurationChange.
        auto prevInfos = usbChannelInfos;
        refreshBridgeStreamInfo();

        if (usbChannelInfos != prevInfos)
            rebuildPatchbay();
    }

    void onChannelMapReceived (const std::vector<AppBackend::SlotConfig>& slots)
    {
        channelMap = slots;
        syncCombosToMap();
    }

    void syncCombosToMap()
    {
        // channelMap[i] is slot i. usbChannel==0xFF means unassigned.
        for (int i = 0; i < static_cast<int> (patchRows.size()); ++i)
        {
            auto& row = patchRows[static_cast<size_t> (i)];
            if (i >= static_cast<int> (channelMap.size())
                || channelMap[static_cast<size_t> (i)].usbChannel == 0xFF)
            {
                row.usbChannel = -1;
                row.combo->setSelectedId (1000, juce::dontSendNotification);
            }
            else
            {
                int usbCh = static_cast<int> (channelMap[static_cast<size_t> (i)].usbChannel);
                row.usbChannel = usbCh;
                row.combo->setSelectedId (usbCh + 1, juce::dontSendNotification);
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

        int rowCount = usbChannelInfos.empty()
                       ? bridgeChannelCount
                       : static_cast<int> (usbChannelInfos.size());
        rowCount = std::min (rowCount, maxBridgeChannels);

        // Build the full initial slot table and send ONE broadcast to the driver.
        // Previously this was resetSlots(n) + N×setSlot(), causing N+1 rapid
        // applyChannelMap() calls in the driver — each triggering RequestConfigurationChange
        // and a USB reader stop/start cycle, leading to race conditions and no audio.
        {
            std::vector<AppBackend::SlotConfig> slots;
            slots.reserve (static_cast<size_t> (rowCount));
            for (int ch = 0; ch < rowCount; ++ch)
            {
                AppBackend::SlotConfig s;
                if (ch < static_cast<int> (usbChannelInfos.size()))
                {
                    auto const& info = usbChannelInfos[static_cast<size_t> (ch)];
                    s.usbChannel  = info.channelIndex;
                    s.entityId    = info.talkerEntityId;
                    s.displayName = info.sourceName;
                }
                slots.push_back (std::move (s));
            }
            backend.initializeSlots (slots);
        }

        for (int ch = 0; ch < rowCount; ++ch)
        {
            PatchRow row;
            row.usbChannel = (ch < static_cast<int> (usbChannelInfos.size())) ? ch : -1;
            row.label = std::make_unique<juce::Label> ("", "Ch " + juce::String (ch + 1));
            row.combo = std::make_unique<juce::ComboBox>();
            row.combo->addItem ("(none)", 1000);

            for (auto const& info : usbChannelInfos)
            {
                juce::String label = "Channel " + juce::String (info.channelIndex + 1);
                label += info.sourceName.empty() ? " - no source"
                                                 : " - " + juce::String (info.sourceName);
                row.combo->addItem (label, info.channelIndex + 1);
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

    juce::Label    labelNetif    { "", "Network Interface:" };
    juce::ComboBox comboNetif;
    juce::Label    labelBridge  { "", "USB Bridge (CoreAudio):" };
    juce::ComboBox comboBridge;
    juce::Label    labelListener { "", "AVDECC Listener (bridge entity):" };
    juce::ComboBox comboListener;
    juce::Label    labelStatus  { "", "Status: starting..." };
    juce::Label    labelDriver  { "", "Driver: checking..." };

    // Patchbay: one row per USB channel slot.
    struct PatchRow
    {
        int usbChannel;
        std::unique_ptr<juce::Label>    label;
        std::unique_ptr<juce::ComboBox> combo;
    };

    std::vector<PatchRow> patchRows;
    std::vector<GLAEntityInfo> entities;
    std::vector<AppBackend::SlotConfig> channelMap;
    std::vector<AppBackend::USBChannelInfo> usbChannelInfos;

    int      bridgeChannelCount    = 0;
    uint64_t bridgeListenerEntityId = 0;

    std::vector<std::string>  netifBSDNames;
    std::vector<std::string>  bridgeUIDs;
    std::vector<std::string>  bridgeNames;
    std::vector<int>          bridgeChannelCounts;
    std::vector<uint64_t>     listenerEntityIds;

    //==============================================================================
    // Enumerate CoreAudio input devices (potential USB bridges).
    // Returns {name, uid, inputChannelCount} tuples.
    static std::vector<std::tuple<std::string, std::string, int>> getAudioInputDevices()
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

        std::vector<std::tuple<std::string, std::string, int>> result; // {name, uid, channels}

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

            result.emplace_back (getStr (kAudioObjectPropertyName), uid, channels);
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
