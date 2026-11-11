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

    struct LabelledSlider
    {
        juce::Label label;
        juce::Slider slider;
        std::unique_ptr<SliderAttachment> attachment;
    };

    ScopeComponent scope;

    juce::ComboBox modeBox, colourBox, layoutBox;
    juce::ToggleButton testButton { "Test tone" };

    // Row 2: signal controls. Row 3: image controls.
    LabelledSlider window, delay, afterglow, brightness, contrast, blur;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment;
    std::unique_ptr<ButtonAttachment> testAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeEditor)
};
