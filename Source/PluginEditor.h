#pragma once

#include "PluginProcessor.h"
#include "ScopeComponent.h"
#include "ControlPanel.h"
#include "FullScreenWindow.h"

class CircleScopeEditor : public juce::AudioProcessorEditor
{
public:
    explicit CircleScopeEditor (CircleScopeProcessor&);
    ~CircleScopeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void toggleFullScreen();

    CircleScopeProcessor& scopeProcessor;
    ScopeComponent scope;
    juce::TextButton fullScreenButton { "Full screen" };
    ControlPanel controls;
    std::unique_ptr<FullScreenWindow> fullScreen;   // declared last: destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeEditor)
};
