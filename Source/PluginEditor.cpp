#include "PluginEditor.h"

CircleScopeEditor::CircleScopeEditor (CircleScopeProcessor& p)
    : AudioProcessorEditor (p), scope (p)
{
    addAndMakeVisible (scope);

    modeBox.addItemList ({ "Quadrature", "Delay embed" }, 1);
    colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
    layoutBox.addItemList ({ "Side by side", "Overlay", "Mid / side" }, 1);

    for (auto* s : { &windowSlider, &delaySlider, &afterglowSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s->setTextValueSuffix (" ms");
    }

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &modeBox, &colourBox, &layoutBox, &windowSlider, &delaySlider, &afterglowSlider,
             &windowLabel, &delayLabel, &afterglowLabel, &testButton })
        addAndMakeVisible (c);

    auto& apvts = p.apvts;
    modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
    colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
    layoutAttachment = std::make_unique<ComboAttachment> (apvts, "layout", layoutBox);
    windowAttachment = std::make_unique<SliderAttachment> (apvts, "window", windowSlider);
    delayAttachment = std::make_unique<SliderAttachment> (apvts, "delay", delaySlider);
    afterglowAttachment = std::make_unique<SliderAttachment> (apvts, "afterglow", afterglowSlider);
    testAttachment = std::make_unique<ButtonAttachment> (apvts, "test", testButton);

    setResizable (true, true);
    setResizeLimits (620, 360, 2400, 1400);
    setSize (960, 580);
}

void CircleScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17171a));
}

void CircleScopeEditor::resized()
{
    auto area = getLocalBounds();
    auto controls = area.removeFromBottom (76).reduced (10, 6);

    auto row1 = controls.removeFromTop (30).reduced (0, 3);
    for (auto* box : { &modeBox, &colourBox, &layoutBox })
    {
        box->setBounds (row1.removeFromLeft (140));
        row1.removeFromLeft (8);
    }
    testButton.setBounds (row1.removeFromRight (100));

    auto row2 = controls.removeFromTop (30).reduced (0, 3);
    const int third = row2.getWidth() / 3;
    auto place = [] (juce::Rectangle<int> r, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (r.removeFromLeft (70));
        slider.setBounds (r.reduced (4, 0));
    };
    place (row2.removeFromLeft (third), windowLabel, windowSlider);
    place (row2.removeFromLeft (third), delayLabel, delaySlider);
    place (row2, afterglowLabel, afterglowSlider);

    scope.setBounds (area);
}
