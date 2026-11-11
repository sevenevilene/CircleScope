#include "PluginEditor.h"

CircleScopeEditor::CircleScopeEditor (CircleScopeProcessor& p)
    : AudioProcessorEditor (p), scope (p)
{
    addAndMakeVisible (scope);

    modeBox.addItemList ({ "Quadrature", "Delay embed" }, 1);
    colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
    layoutBox.addItemList ({ "Side by side", "Overlay", "Mid / side" }, 1);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &modeBox, &colourBox, &layoutBox, &testButton })
        addAndMakeVisible (c);

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

    setResizable (true, true);
    setResizeLimits (640, 400, 2400, 1400);
    setSize (980, 620);
}

void CircleScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17171a));
}

void CircleScopeEditor::resized()
{
    auto area = getLocalBounds();
    auto controls = area.removeFromBottom (106).reduced (10, 6);

    auto row1 = controls.removeFromTop (30).reduced (0, 3);
    for (auto* box : { &modeBox, &colourBox, &layoutBox })
    {
        box->setBounds (row1.removeFromLeft (140));
        row1.removeFromLeft (8);
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

    scope.setBounds (area);
}
