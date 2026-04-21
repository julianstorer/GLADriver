#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AppBackend.hpp"
#include "../common/gla_ipc_types.hpp"
#include <vector>

//==============================================================================
// Main UI component.
// Shows: Network Interface | USB Bridge | Entity-to-channel patchbay | Status label.
class MainComponent : public juce::Component,
                      public juce::ComboBox::Listener,
                      public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    void comboBoxChanged (juce::ComboBox* box) override;
    void timerCallback() override;

private:
    //==============================================================================
    void refreshNetworkInterfaces();
    void onEntityListReceived (const std::vector<GLAEntityInfo>& entities);
    void onChannelMapReceived (const std::vector<GLAChannelEntry>& entries);
    void rebuildPatchbay();

    //==============================================================================
    AppBackend backend;

    juce::Label    labelNetif  { "", "Network Interface:" };
    juce::ComboBox comboNetif;
    juce::Label    labelBridge { "", "USB Bridge:" };
    juce::ComboBox comboBridge;
    juce::Label    labelStatus { "", "Status: starting..." };

    // Patchbay: one row per USB channel slot.
    struct PatchRow
    {
        int usbChannel;
        std::unique_ptr<juce::Label>    label;
        std::unique_ptr<juce::ComboBox> combo;
    };
    std::vector<PatchRow> patchRows;

    std::vector<GLAEntityInfo>   entities;
    std::vector<GLAChannelEntry> channelMap;

    int bridgeChannelCount = 0;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
