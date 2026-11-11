#include "PluginEditor.h"

CircleScopeEditor::CircleScopeEditor (CircleScopeProcessor& p)
    : AudioProcessorEditor (p), scope (p)
{
    addAndMakeVisible (scope);

    modeBox.addItemList ({ "Quadrature", "Delay embed" }, 1);
    colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);

    for (auto* s : { &windowSlider, &delaySlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s->setTextValueSuffix (" ms");
    }

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &modeBox, &colourBox, &windowSlider, &delaySlider, &windowLabel, &delayLabel, &testButton })
        addAndMakeVisible (c);

    auto& apvts = p.apvts;
    modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
    colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
    windowAttachment = std::make_unique<SliderAttachment> (apvts, "window", windowSlider);
    delayAttachment = std::make_unique<SliderAttachment> (apvts, "delay", delaySlider);
    testAttachment = std::make_unique<ButtonAttachment> (apvts, "test", testButton);

    setResizable (true, true);
    setResizeLimits (520, 320, 2400, 1400);
    setSize (960, 540);
}

void CircleScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17171a));
}

void CircleScopeEditor::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromBottom (44).reduced (10, 10);

    modeBox.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (8);
    colourBox.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (8);
    testButton.setBounds (bar.removeFromRight (100));

    const int half = bar.getWidth() / 2;
    auto left = bar.removeFromLeft (half);
    windowLabel.setBounds (left.removeFromLeft (56));
    windowSlider.setBounds (left.reduced (4, 0));
    delayLabel.setBounds (bar.removeFromLeft (48));
    delaySlider.setBounds (bar.reduced (4, 0));

    scope.setBounds (area);
}
