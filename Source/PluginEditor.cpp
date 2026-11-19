#include "PluginEditor.h"

CircleScopeEditor::CircleScopeEditor (CircleScopeProcessor& p)
    : AudioProcessorEditor (p),
      scopeProcessor (p),
      scope (p.feed, true),
      cameraPad (p.apvts),
      controls (p, &fullScreenButton, false)
{
    addAndMakeVisible (scope);
    scope.addAndMakeVisible (cameraPad);
    addAndMakeVisible (controls);
    controls.onHeightChanged = [this] { resized(); };

    fullScreenButton.setTooltip ("Full screen (or double-click the scope). Esc or F exits.");
    fullScreenButton.onClick = [this] { toggleFullScreen(); };
    scope.onDoubleClick = [this] { toggleFullScreen(); };

    setResizable (true, true);
    setResizeLimits (800, 830, 2400, 1500);
    setSize (980, 1040);

    startTimerHz (5);
    timerCallback();
}

// While any instance in the host is in full screen, no editor draws its
// scope: this one's, or another instance's that is open beside it.
void CircleScopeEditor::timerCallback()
{
    scope.setVisible (FullScreenWindow::numShowing.load() == 0);
}

CircleScopeEditor::~CircleScopeEditor()
{
    stopTimer();
    fullScreen.reset();   // leave kiosk mode before the editor goes away
}

void CircleScopeEditor::toggleFullScreen()
{
    if (fullScreen != nullptr)
    {
        fullScreen.reset();
        fullScreenButton.setButtonText ("Full screen");
        timerCallback();
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

    timerCallback();
}

void CircleScopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff09090b));
}

void CircleScopeEditor::resized()
{
    auto area = getLocalBounds();
    controls.setBounds (area.removeFromBottom (controls.getPreferredHeight()));
    scope.setBounds (area);
    cameraPad.setTopLeftPosition (8, scope.getHeight() - CameraPad::height - 8);
}
