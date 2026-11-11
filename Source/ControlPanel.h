#pragma once

#include "PluginProcessor.h"

// All parameter controls, attached to the processor's parameters. Used both
// in the editor and as an overlay in full screen; several panels can exist
// at once and stay in sync through the parameter attachments.
class ControlPanel : public juce::Component
{
public:
    // trailing: optional component placed at the right end of the top row.
    // translucent: draw a dark see-through backing, for use over the scope.
    ControlPanel (CircleScopeProcessor& p, juce::Component* trailing, bool translucent)
        : trailingComponent (trailing), isTranslucent (translucent)
    {
        modeBox.addItemList ({ "Quadrature", "Delay embed" }, 1);
        colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
        layoutBox.addItemList ({ "Side by side", "Overlay", "Mid / side" }, 1);

        for (juce::Component* c : std::initializer_list<juce::Component*> { &modeBox, &colourBox, &layoutBox, &testButton })
            addAndMakeVisible (c);
        if (trailingComponent != nullptr)
            addAndMakeVisible (trailingComponent);

        auto& apvts = p.apvts;
        auto setUp = [this, &apvts] (LabelledSlider& ls, const char* paramId, const char* name, const char* suffix)
        {
            ls.label.setText (name, juce::dontSendNotification);
            ls.slider.setSliderStyle (juce::Slider::LinearHorizontal);
            ls.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            ls.slider.setTextValueSuffix (suffix);
            ls.attachment = std::make_unique<SliderAttachment> (apvts, paramId, ls.slider);
            addAndMakeVisible (ls.label);
            addAndMakeVisible (ls.slider);
        };

        setUp (window, "window", "Window", " ms");
        setUp (delay, "delay", "Delay", " ms");
        setUp (afterglow, "afterglow", "Afterglow", " ms");
        setUp (brightness, "brightness", "Brightness", " %");
        setUp (contrast, "contrast", "Contrast", " %");
        setUp (blur, "blur", "Blur", " px");

        modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
        colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
        layoutAttachment = std::make_unique<ComboAttachment> (apvts, "layout", layoutBox);
        testAttachment = std::make_unique<ButtonAttachment> (apvts, "test", testButton);
    }

    static constexpr int preferredHeight = 106;

    void paint (juce::Graphics& g) override
    {
        if (isTranslucent)
        {
            g.setColour (juce::Colour (0xff17171a).withAlpha (0.78f));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
        }
        else
        {
            g.fillAll (juce::Colour (0xff17171a));
        }
    }

    void resized() override
    {
        auto controls = getLocalBounds().reduced (10, 6);

        auto row1 = controls.removeFromTop (30).reduced (0, 3);
        for (auto* box : { &modeBox, &colourBox, &layoutBox })
        {
            box->setBounds (row1.removeFromLeft (140));
            row1.removeFromLeft (8);
        }
        if (trailingComponent != nullptr)
        {
            trailingComponent->setBounds (row1.removeFromRight (124));
            row1.removeFromRight (8);
        }
        testButton.setBounds (row1.removeFromRight (100));

        auto placeRow = [] (juce::Rectangle<int> row, std::initializer_list<LabelledSlider*> items)
        {
            const int third = row.getWidth() / 3;
            for (auto* ls : items)
            {
                auto cell = row.removeFromLeft (third);
                ls->label.setBounds (cell.removeFromLeft (76));
                ls->slider.setBounds (cell.reduced (4, 0));
            }
        };
        placeRow (controls.removeFromTop (30).reduced (0, 3), { &window, &delay, &afterglow });
        placeRow (controls.removeFromTop (30).reduced (0, 3), { &brightness, &contrast, &blur });
    }

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

    juce::Component* trailingComponent;
    const bool isTranslucent;

    juce::ComboBox modeBox, colourBox, layoutBox;
    juce::ToggleButton testButton { "Test tone" };

    // Row 2: signal controls. Row 3: image controls.
    LabelledSlider window, delay, afterglow, brightness, contrast, blur;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment;
    std::unique_ptr<ButtonAttachment> testAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};
