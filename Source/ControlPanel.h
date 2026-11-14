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
        modeBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pMode].choices, "|", ""), 1);
        swingMotionBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pSwingMotion].choices, "|", ""), 1);
        swingMotionBox.setTooltip ("Swing mode: the axis swings back and forth like a pendulum, or turns round and round");
        swingSyncButton.setTooltip ("Swing mode: one swing per Window length. Off: the Swing rate sets it");
        swingLabel.setText ("Swing", juce::dontSendNotification);
        colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
        layoutBox.addItemList ({ "Side by side", "Overlay", "Mid / side" }, 1);

        blendBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pBlend].choices, "|", ""), 1);
        layerBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pLayer].choices, "|", ""), 1);
        blendBox.setTooltip ("On a wall or cluster node: how this instance is combined with the layers under it");
        layerBox.setTooltip ("Stacking order among the instances on the same stream. 1 is the bottom layer");
        divideButton.setTooltip ("On a wall: split one picture over the cells of all the streams switched on here, instead of repeating it in each");
        blendLabel.setText ("Blend", juce::dontSendNotification);
        layerLabel.setText ("Layer", juce::dontSendNotification);
        streamLabel.setText ("Streams", juce::dontSendNotification);
        streamStatus.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));

        for (int i = 0; i < net::maxStreams; ++i)
        {
            auto& button = streamButtons[(size_t) i];
            button.setButtonText (juce::String (i + 1));
            button.setClickingTogglesState (true);
            button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff7f77dd));
            button.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < net::maxStreams - 1 ? juce::Button::ConnectedOnRight : 0));
            button.setTooltip ("Send this instance to stream " + juce::String (i + 1)
                               + ". On a CircleScope Wall a stream is a cell; instances sharing one are layers");
            addAndMakeVisible (button);
        }

        for (juce::Component* c : std::initializer_list<juce::Component*> { &modeBox, &colourBox, &layoutBox, &guidesButton, &testButton,
                                                                            &swingLabel, &swingMotionBox, &swingSyncButton,
                                                                            &blendLabel, &blendBox, &layerLabel, &layerBox,
                                                                            &streamLabel, &divideButton, &streamStatus })
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
        setUp (swingRate, "swingrate", "Rate", " Hz");

        modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
        colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
        layoutAttachment = std::make_unique<ComboAttachment> (apvts, "layout", layoutBox);
        swingMotionAttachment = std::make_unique<ComboAttachment> (apvts, "swingmotion", swingMotionBox);
        swingSyncAttachment = std::make_unique<ButtonAttachment> (apvts, "swingsync", swingSyncButton);
        blendAttachment = std::make_unique<ComboAttachment> (apvts, "blend", blendBox);
        layerAttachment = std::make_unique<ComboAttachment> (apvts, "layer", layerBox);
        divideAttachment = std::make_unique<ButtonAttachment> (apvts, "divide", divideButton);
        for (int i = 0; i < net::maxStreams; ++i)
            streamAttachments[(size_t) i] = std::make_unique<ButtonAttachment> (apvts, "netstream" + juce::String (i + 1),
                                                                                streamButtons[(size_t) i]);
        guidesAttachment = std::make_unique<ButtonAttachment> (apvts, "guides", guidesButton);
        testAttachment = std::make_unique<ButtonAttachment> (apvts, "test", testButton);

        startTimerHz (8);
        timerCallback();
    }

    static constexpr int preferredHeight = 196;

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

        // Row 4: band limits, then how this instance layers in the third cell.
        auto row4 = controls.removeFromTop (30).reduced (0, 3);
        auto layering = row4.removeFromRight (row4.getWidth() / 3);
        placeRow (row4.withWidth (row4.getWidth() * 3 / 2), { &lowCut, &highCut });
        blendLabel.setBounds (layering.removeFromLeft (76));
        layerBox.setBounds (layering.removeFromRight (64).reduced (4, 0));
        layerLabel.setBounds (layering.removeFromRight (46));
        blendBox.setBounds (layering.reduced (4, 0));

        // Row 5: the axis of the Swing mode.
        auto row5swing = controls.removeFromTop (30).reduced (0, 3);
        const int third = row5swing.getWidth() / 3;
        auto motion = row5swing.removeFromLeft (third);
        swingLabel.setBounds (motion.removeFromLeft (76));
        swingMotionBox.setBounds (motion.reduced (4, 0));
        auto rate = row5swing.removeFromLeft (third);
        swingRate.label.setBounds (rate.removeFromLeft (76));
        swingRate.slider.setBounds (rate.reduced (4, 0));
        swingSyncButton.setBounds (row5swing.removeFromLeft (150).reduced (4, 0));

        // Row 6: the streams this instance is sent to.
        auto row5 = controls.removeFromTop (30).reduced (0, 3);
        streamLabel.setBounds (row5.removeFromLeft (76));
        row5.removeFromLeft (4);
        for (auto& button : streamButtons)
            button.setBounds (row5.removeFromLeft (30));
        row5.removeFromLeft (12);
        divideButton.setBounds (row5.removeFromLeft (80));
        streamStatus.setBounds (row5.reduced (4, 0));
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
        // The swing controls only do something in Swing mode, the rate only when it is free.
        const bool swing = modeBox.getSelectedId() == (int) scope::Mode::swing + 1;
        swingMotionBox.setEnabled (swing);
        swingSyncButton.setEnabled (swing);
        swingRate.slider.setEnabled (swing && ! swingSyncButton.getToggleState());

        bool sending = false;
        for (auto& button : streamButtons)
            sending = sending || button.getToggleState();
        const int nodes = processor.getNumNetworkTargets();
        streamStatus.setText (! sending ? juce::String()
                                        : nodes == 0 ? juce::String ("nothing listening")
                                                     : juce::String (nodes) + (nodes == 1 ? " receiver" : " receivers"),
                              juce::dontSendNotification);
    }

    CircleScopeProcessor& processor;
    juce::Component* trailingComponent;
    const bool isTranslucent;

    juce::ComboBox modeBox, colourBox, layoutBox, blendBox, layerBox, swingMotionBox;
    juce::ToggleButton guidesButton { "Guides" }, testButton { "Test tone" }, divideButton { "Divide" },
                       swingSyncButton { "Once per window" };
    juce::Label blendLabel, layerLabel, streamLabel, streamStatus, swingLabel;
    std::array<juce::TextButton, net::maxStreams> streamButtons;

    // Row 2: signal controls. Row 3: image controls. Row 4: band limits, layering. Row 5: swing. Row 6: streams.
    LabelledSlider window, delay, afterglow, brightness, contrast, blur, lowCut, highCut, swingRate;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment, blendAttachment, layerAttachment, swingMotionAttachment;
    std::unique_ptr<ButtonAttachment> guidesAttachment, testAttachment, divideAttachment, swingSyncAttachment;
    std::array<std::unique_ptr<ButtonAttachment>, net::maxStreams> streamAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};
