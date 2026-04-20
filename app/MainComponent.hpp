#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "DaemonClient.hpp"
#include "../common/gla_ipc_types.hpp"
#include <vector>
#include <string>

// Main UI component.
// Shows: Network Interface | USB Bridge | Entity-to-channel patchbay | Status label.
class MainComponent : public juce::Component,
                      public juce::ComboBox::Listener,
                      public juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void comboBoxChanged(juce::ComboBox* box) override;
    void timerCallback() override;

private:
    void refreshNetworkInterfaces();
    void onEntityListReceived(const std::vector<GLAEntityInfo>& entities);
    void onChannelMapReceived(const std::vector<GLAChannelEntry>& entries);
    void rebuildPatchbay();

    DaemonClient _client;

    juce::Label    _labelNetif{"", "Network Interface:"};
    juce::ComboBox _comboNetif;
    juce::Label    _labelBridge{"", "USB Bridge:"};
    juce::ComboBox _comboBridge;
    juce::Label    _labelStatus{"", "Status: connecting..."};

    // Patchbay: one row per USB channel slot.
    // Each row: label "Ch N" + ComboBox (entity selector).
    struct PatchRow {
        int usbChannel;
        std::unique_ptr<juce::Label>    label;
        std::unique_ptr<juce::ComboBox> combo;
    };
    std::vector<PatchRow> _patchRows;

    std::vector<GLAEntityInfo>  _entities;
    std::vector<GLAChannelEntry> _channelMap;

    // USB bridge input channel count (determines patchbay row count).
    int _bridgeChannelCount = 0;
};
