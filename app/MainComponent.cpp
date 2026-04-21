#include "MainComponent.hpp"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static constexpr int kMaxBridgeChannels = 32;
static constexpr int kRowHeight = 28;
[[maybe_unused]] static constexpr int kTopPanelHeight = 120;

// Enumerate CoreAudio input devices (potential USB bridges).
static std::vector<std::pair<std::string, std::string>> getAudioInputDevices() {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    std::vector<AudioDeviceID> ids(dataSize / sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize,
                               ids.data());

    std::vector<std::pair<std::string, std::string>> result; // {name, uid}
    for (auto id : ids) {
        // Check input channels.
        AudioObjectPropertyAddress inputProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(id, &inputProp, 0, nullptr, &sz) != noErr) continue;
        std::vector<uint8_t> buf(sz);
        if (AudioObjectGetPropertyData(id, &inputProp, 0, nullptr, &sz, buf.data()) != noErr) continue;
        auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
        int channels = 0;
        for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
            channels += static_cast<int>(list->mBuffers[b].mNumberChannels);
        if (channels < 1) continue;

        auto getStr = [&](AudioObjectPropertySelector sel) -> std::string {
            AudioObjectPropertyAddress sp = { sel, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
            CFStringRef cf = nullptr;
            UInt32 s = sizeof(cf);
            if (AudioObjectGetPropertyData(id, &sp, 0, nullptr, &s, &cf) != noErr || !cf)
                return "";
            char buf2[256] = {};
            CFStringGetCString(cf, buf2, sizeof(buf2), kCFStringEncodingUTF8);
            CFRelease(cf);
            return buf2;
        };
        result.push_back({getStr(kAudioObjectPropertyName),
                          getStr(kAudioDevicePropertyDeviceUID)});
    }
    return result;
}

// Network interface names via getifaddrs.
#include <ifaddrs.h>
#include <net/if.h>
static std::vector<std::string> getNetworkInterfaces() {
    std::vector<std::string> result;
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return result;
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        std::string name = ifa->ifa_name;
        if (std::find(result.begin(), result.end(), name) == result.end())
            result.push_back(name);
    }
    freeifaddrs(ifap);
    return result;
}

MainComponent::MainComponent() {
    setSize(600, 500);

    addAndMakeVisible(_labelNetif);
    addAndMakeVisible(_comboNetif);
    addAndMakeVisible(_labelBridge);
    addAndMakeVisible(_comboBridge);
    addAndMakeVisible(_labelStatus);

    _comboNetif.addListener(this);
    _comboBridge.addListener(this);

    refreshNetworkInterfaces();

    _client.setEntityListCallback([this](const std::vector<GLAEntityInfo>& e) {
        onEntityListReceived(e);
    });
    _client.setChannelMapCallback([this](const std::vector<GLAChannelEntry>& m) {
        onChannelMapReceived(m);
    });

    startTimer(5000); // re-scan audio devices every 5s
}

MainComponent::~MainComponent() {
    stopTimer();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    g.setColour(juce::Colours::white);
    g.setFont(14.0f);
    g.drawText("GLA Injector - Patchbay", 10, 10, getWidth() - 20, 24,
               juce::Justification::centredLeft);
}

void MainComponent::resized() {
    auto area = getLocalBounds().reduced(10);
    area.removeFromTop(30); // title

    _labelNetif.setBounds(area.removeFromTop(20));
    _comboNetif.setBounds(area.removeFromTop(kRowHeight));
    area.removeFromTop(4);
    _labelBridge.setBounds(area.removeFromTop(20));
    _comboBridge.setBounds(area.removeFromTop(kRowHeight));
    area.removeFromTop(8);
    _labelStatus.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);

    for (auto& row : _patchRows) {
        auto rowArea = area.removeFromTop(kRowHeight);
        row.label->setBounds(rowArea.removeFromLeft(60));
        row.combo->setBounds(rowArea);
        area.removeFromTop(2);
    }
}

void MainComponent::comboBoxChanged(juce::ComboBox* box) {
    if (box == &_comboNetif) {
        auto iface = _comboNetif.getText().toStdString();
        if (!iface.empty()) _client.sendSetNetworkInterface(iface);
    } else if (box == &_comboBridge) {
        auto uid = _comboBridge.getItemText(_comboBridge.getSelectedItemIndex())
                               .toStdString();
        // Store UID as item tooltip isn't easily available; use a parallel list.
        // For simplicity, set bridge by UID stored in combo item ID.
        // We use item ID = index+1, and store UIDs in a parallel vector.
        // TODO: refine by storing UID as item text or using a custom model.
        _client.sendSetUSBBridge(uid);
    } else {
        // It's a patchbay row combo.
        for (int ch = 0; ch < static_cast<int>(_patchRows.size()); ++ch) {
            if (_patchRows[static_cast<size_t>(ch)].combo.get() == box) {
                int selectedId = box->getSelectedId();
                if (selectedId > 0 &&
                    static_cast<size_t>(selectedId - 1) < _entities.size()) {
                    auto entityId = _entities[static_cast<size_t>(selectedId - 1)].entity_id;
                    _client.sendSetRouting(static_cast<uint8_t>(ch), entityId);
                }
                break;
            }
        }
    }
}

void MainComponent::timerCallback() {
    refreshNetworkInterfaces();
    // Refresh USB bridge list.
    auto devices = getAudioInputDevices();
    int current = _comboBridge.getSelectedId();
    _comboBridge.clear(juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
        _comboBridge.addItem(devices[static_cast<size_t>(i)].first + " (" +
                             devices[static_cast<size_t>(i)].second + ")", i + 1);
    }
    _comboBridge.setSelectedId(current, juce::dontSendNotification);
}

void MainComponent::refreshNetworkInterfaces() {
    auto ifaces = getNetworkInterfaces();
    int current = _comboNetif.getSelectedId();
    _comboNetif.clear(juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(ifaces.size()); ++i) {
        _comboNetif.addItem(ifaces[static_cast<size_t>(i)], i + 1);
    }
    _comboNetif.setSelectedId(current > 0 ? current : 1, juce::dontSendNotification);
}

void MainComponent::onEntityListReceived(const std::vector<GLAEntityInfo>& entities) {
    _entities = entities;
    _labelStatus.setText(juce::String(static_cast<int>(entities.size())) +
                         " entities on network", juce::dontSendNotification);
    rebuildPatchbay();
}

void MainComponent::onChannelMapReceived(const std::vector<GLAChannelEntry>& entries) {
    _channelMap = entries;
    // Update patchbay selections.
    for (auto const& e : entries) {
        int ch = e.channel_index;
        if (ch >= static_cast<int>(_patchRows.size())) continue;
        // Find entity index.
        for (int i = 0; i < static_cast<int>(_entities.size()); ++i) {
            if (_entities[static_cast<size_t>(i)].entity_id == e.entity_id) {
                _patchRows[static_cast<size_t>(ch)].combo->setSelectedId(
                    i + 1, juce::dontSendNotification);
                break;
            }
        }
    }
}

void MainComponent::rebuildPatchbay() {
    for (auto& row : _patchRows) {
        removeChildComponent(row.label.get());
        removeChildComponent(row.combo.get());
    }
    _patchRows.clear();

    int rowCount = (_bridgeChannelCount > 0) ? _bridgeChannelCount : 8;
    rowCount = std::min(rowCount, kMaxBridgeChannels);

    for (int ch = 0; ch < rowCount; ++ch) {
        PatchRow row;
        row.usbChannel = ch;
        row.label = std::make_unique<juce::Label>("", "Ch " + juce::String(ch + 1));
        row.combo = std::make_unique<juce::ComboBox>();
        row.combo->addItem("(none)", 1000);
        for (int i = 0; i < static_cast<int>(_entities.size()); ++i) {
            auto& ent = _entities[static_cast<size_t>(i)];
            juce::String name = ent.name;
            if (!ent.online) name = "[offline] " + name;
            row.combo->addItem(name, i + 1);
        }
        row.combo->addListener(this);
        addAndMakeVisible(*row.label);
        addAndMakeVisible(*row.combo);
        _patchRows.push_back(std::move(row));
    }

    resized();
}
