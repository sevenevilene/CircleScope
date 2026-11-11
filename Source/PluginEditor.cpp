#include "PluginEditor.h"

CircleScopeEditor::CircleScopeEditor (CircleScopeProcessor& p)
    : AudioProcessorEditor (p),
      scopeProcessor (p),
      scope (p),
      controls (p, &fullScreenButton, false)
{
    addAndMakeVisible (scope);
    addAndMakeVisible (controls);

    fullScreenButton.setTooltip ("Full screen (or double-click the scope)");
    fullScreenButton.onClick = [this] { toggleFullScreen(); };
    scope.onDoubleClick = [this] { toggleFullScreen(); };

    setResizable (true, true);
    setResizeLimits (640, 400, 2400, 1400);
    setSize (980, 620);
}

CircleScopeEditor::~CircleScopeEditor()
{
    fullScreen.reset();   // leave kiosk mode before the editor goes away
}

void CircleScopeEditor::toggleFullScreen()
{
    if (fullScreen != nullptr)
    {
        fullScreen.reset();
        fullScreenButton.setButtonText ("Full screen");
        return;
    }

    // Fill whichever display the editor is on.
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
    if (display == nullptr)
        return;

    juce::Component::SafePointer<CircleScopeEditor> safeThis (this);
    fullScreen = std::make_unique<FullScreenWindow> (
        scopeProcessor, display->totalArea,
        [safeThis] { if (safeThis != nullptr && safeThis->fullScreen != nullptr) safeThis->toggleFullScreen(); });
    fullScreenButton.setButtonText ("Exit full screen");
}

void CircleScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17171a));
}

void CircleScopeEditor::resized()
{
    auto area = getLocalBounds();
    controls.setBounds (area.removeFromBottom (ControlPanel::preferredHeight));
    scope.setBounds (area);
}
