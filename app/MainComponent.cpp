#include "MainComponent.hpp"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static constexpr int maxBridgeChannels = 32;
static constexpr int rowHeight         = 28;

// Enumerate CoreAudio input devices (potential USB bridges).
static std::vector<std::pair<std::string, std::string>> getAudioInputDevices()
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    std::vector<AudioDeviceID> ids (dataSize / sizeof (AudioDeviceID));
    AudioObjectGetPropertyData (kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize,
                                ids.data());

    std::vector<std::pair<std::string, std::string>> result; // {name, uid}
    for (auto id : ids)
    {
        AudioObjectPropertyAddress inputProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize (id, &inputProp, 0, nullptr, &sz) != noErr) continue;
        std::vector<uint8_t> buf (sz);
        if (AudioObjectGetPropertyData (id, &inputProp, 0, nullptr, &sz, buf.data()) != noErr) continue;
        auto* list = reinterpret_cast<AudioBufferList*> (buf.data());
        int channels = 0;
        for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
            channels += static_cast<int> (list->mBuffers[b].mNumberChannels);
        if (channels < 1) continue;

        auto getStr = [&] (AudioObjectPropertySelector sel) -> std::string
        {
            AudioObjectPropertyAddress sp = { sel, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
            CFStringRef cf = nullptr;
            UInt32 s = sizeof (cf);
            if (AudioObjectGetPropertyData (id, &sp, 0, nullptr, &s, &cf) != noErr || !cf)
                return "";
            char buf2[256] = {};
            CFStringGetCString (cf, buf2, sizeof (buf2), kCFStringEncodingUTF8);
            CFRelease (cf);
            return buf2;
        };
        result.push_back ({ getStr (kAudioObjectPropertyName),
                            getStr (kAudioDevicePropertyDeviceUID) });
    }
    return result;
}

#include <ifaddrs.h>
#include <net/if.h>
static std::vector<std::string> getNetworkInterfaces()
{
    std::vector<std::string> result;
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs (&ifap) != 0) return result;
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        std::string name = ifa->ifa_name;
        if (std::find (result.begin(), result.end(), name) == result.end())
            result.push_back (name);
    }
    freeifaddrs (ifap);
    return result;
}

//==============================================================================
MainComponent::MainComponent()
{
    setSize (600, 500);

    addAndMakeVisible (labelNetif);
    addAndMakeVisible (comboNetif);
    addAndMakeVisible (labelBridge);
    addAndMakeVisible (comboBridge);
    addAndMakeVisible (labelStatus);

    comboNetif.addListener (this);
    comboBridge.addListener (this);

    refreshNetworkInterfaces();

    backend.setEntityListCallback ([this] (const std::vector<GLAEntityInfo>& e)
    {
        onEntityListReceived (e);
    });

    auto ifaces = getNetworkInterfaces();
    std::string startIface = ifaces.empty() ? "en0" : ifaces.front();
    if (!backend.start (startIface))
        labelStatus.setText ("Status: IPC server failed to start", juce::dontSendNotification);

    startTimer (5000);
}

MainComponent::~MainComponent()
{
    stopTimer();
    backend.stop();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    g.setColour (juce::Colours::white);
    g.setFont (14.0f);
    g.drawText ("GLA Injector - Patchbay", 10, 10, getWidth() - 20, 24,
                juce::Justification::centredLeft);
}

void MainComponent::resized()
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
    area.removeFromTop (8);

    for (auto& row : patchRows)
    {
        auto rowArea = area.removeFromTop (rowHeight);
        row.label->setBounds (rowArea.removeFromLeft (60));
        row.combo->setBounds (rowArea);
        area.removeFromTop (2);
    }
}

void MainComponent::comboBoxChanged (juce::ComboBox* box)
{
    if (box == &comboNetif)
    {
        auto iface = comboNetif.getText().toStdString();
        if (!iface.empty()) backend.setNetworkInterface (iface);
    }
    else if (box == &comboBridge)
    {
        auto uid = comboBridge.getItemText (comboBridge.getSelectedItemIndex()).toStdString();
        backend.setUSBBridge (uid);
    }
    else
    {
        // It's a patchbay row combo.
        for (int ch = 0; ch < static_cast<int> (patchRows.size()); ++ch)
        {
            if (patchRows[static_cast<size_t> (ch)].combo.get() == box)
            {
                int selectedId = box->getSelectedId();
                if (selectedId > 0 &&
                    static_cast<size_t> (selectedId - 1) < entities.size())
                {
                    auto eid = entities[static_cast<size_t> (selectedId - 1)].entityId;
                    backend.setRouting (static_cast<uint8_t> (ch), eid);
                }
                break;
            }
        }
    }
}

void MainComponent::timerCallback()
{
    refreshNetworkInterfaces();
    auto devices = getAudioInputDevices();
    int current = comboBridge.getSelectedId();
    comboBridge.clear (juce::dontSendNotification);
    for (int i = 0; i < static_cast<int> (devices.size()); ++i)
    {
        comboBridge.addItem (devices[static_cast<size_t> (i)].first + " (" +
                             devices[static_cast<size_t> (i)].second + ")", i + 1);
    }
    comboBridge.setSelectedId (current, juce::dontSendNotification);
}

void MainComponent::refreshNetworkInterfaces()
{
    auto ifaces = getNetworkInterfaces();
    int current = comboNetif.getSelectedId();
    comboNetif.clear (juce::dontSendNotification);
    for (int i = 0; i < static_cast<int> (ifaces.size()); ++i)
        comboNetif.addItem (ifaces[static_cast<size_t> (i)], i + 1);
    comboNetif.setSelectedId (current > 0 ? current : 1, juce::dontSendNotification);
}

void MainComponent::onEntityListReceived (const std::vector<GLAEntityInfo>& newEntities)
{
    entities = newEntities;
    labelStatus.setText (juce::String (static_cast<int> (newEntities.size())) +
                         " entities on network", juce::dontSendNotification);
    rebuildPatchbay();
}

void MainComponent::onChannelMapReceived (const std::vector<GLAChannelEntry>& entries)
{
    channelMap = entries;
    for (auto const& e : entries)
    {
        int ch = e.channelIndex;
        if (ch >= static_cast<int> (patchRows.size())) continue;
        for (int i = 0; i < static_cast<int> (entities.size()); ++i)
        {
            if (entities[static_cast<size_t> (i)].entityId == e.entityId)
            {
                patchRows[static_cast<size_t> (ch)].combo->setSelectedId (
                    i + 1, juce::dontSendNotification);
                break;
            }
        }
    }
}

void MainComponent::rebuildPatchbay()
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
}
