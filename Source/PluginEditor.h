#pragma once

#include "PluginProcessor.h"
#include "ScopeComponent.h"

class CircleScopeEditor : public juce::AudioProcessorEditor
{
public:
    explicit CircleScopeEditor (CircleScopeProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    ScopeComponent scope;

    juce::ComboBox modeBox, colourBox, layoutBox;
    juce::Slider windowSlider, delaySlider, afterglowSlider;
    juce::Label windowLabel { {}, "Window" }, delayLabel { {}, "Delay" }, afterglowLabel { {}, "Afterglow" };
    juce::ToggleButton testButton { "Test tone" };

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment;
    std::unique_ptr<SliderAttachment> windowAttachment, delayAttachment, afterglowAttachment;
    std::unique_ptr<ButtonAttachment> testAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeEditor)
};
