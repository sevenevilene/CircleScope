#pragma once

#include "PluginProcessor.h"

// All parameter controls, attached to the processor's parameters.
class ControlPanel : public juce::Component,
                     private juce::Timer
{
public:
    // trailing: optional component placed at the right end of the top row.
    // translucent: draw a dark see-through backing, for use over the scope.
    ControlPanel (CircleScopeProcessor& p, juce::Component* trailing, bool translucent)
        : processor (p), trailingComponent (trailing), isTranslucent (translucent)
    {
        modeBox.addItemList ({ "Quadrature", "Delay embed" }, 1);
        colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
        layoutBox.addItemList ({ "Side by side", "Overlay", "Mid / side" }, 1);

        streamBox.addItem ("Off", 1);
        for (int i = 1; i <= net::maxStreams; ++i)
            streamBox.addItem ("Stream " + juce::String (i), i + 1);
        streamBox.setTooltip ("Send this instance's audio and settings to cluster nodes showing this stream");
        streamLabel.setText ("Network", juce::dontSendNotification);
        streamStatus.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));

        for (juce::Component* c : std::initializer_list<juce::Component*> { &modeBox, &colourBox, &layoutBox, &guidesButton, &testButton,
                                                                            &streamLabel, &streamBox, &streamStatus })
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
        setUp (lowCut, "lowcut", "Low cut", " Hz");
        setUp (highCut, "highcut", "High cut", " Hz");

        modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
        colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
        layoutAttachment = std::make_unique<ComboAttachment> (apvts, "layout", layoutBox);
        streamAttachment = std::make_unique<ComboAttachment> (apvts, "netstream", streamBox);
        guidesAttachment = std::make_unique<ButtonAttachment> (apvts, "guides", guidesButton);
        testAttachment = std::make_unique<ButtonAttachment> (apvts, "test", testButton);

        startTimerHz (2);
        timerCallback();
    }

    static constexpr int preferredHeight = 136;

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
        guidesButton.setBounds (row1.removeFromRight (80));

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

        // Row 4: band limits, then the network stream in the third cell.
        auto row4 = controls.removeFromTop (30).reduced (0, 3);
        auto network = row4.removeFromRight (row4.getWidth() / 3);
        placeRow (row4.withWidth (row4.getWidth() * 3 / 2), { &lowCut, &highCut });
        streamLabel.setBounds (network.removeFromLeft (76));
        streamBox.setBounds (network.removeFromLeft (110).reduced (4, 0));
        streamStatus.setBounds (network.reduced (4, 0));
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

    void timerCallback() override
    {
        const bool sending = streamBox.getSelectedId() > 1;
        const int nodes = processor.getNumNetworkTargets();
        streamStatus.setText (! sending ? juce::String()
                                        : nodes == 0 ? juce::String ("no nodes listening")
                                                     : juce::String (nodes) + (nodes == 1 ? " node" : " nodes"),
                              juce::dontSendNotification);
    }

    CircleScopeProcessor& processor;
    juce::Component* trailingComponent;
    const bool isTranslucent;

    juce::ComboBox modeBox, colourBox, layoutBox, streamBox;
    juce::ToggleButton guidesButton { "Guides" }, testButton { "Test tone" };
    juce::Label streamLabel, streamStatus;

    // Row 2: signal controls. Row 3: image controls. Row 4: band limits.
    LabelledSlider window, delay, afterglow, brightness, contrast, blur, lowCut, highCut;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment, streamAttachment;
    std::unique_ptr<ButtonAttachment> guidesAttachment, testAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};
