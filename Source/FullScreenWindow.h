#pragma once

#include "ScopeComponent.h"
#include "ControlPanel.h"

// Borderless top-level window that fills one display with its own scope.
// It uses kiosk mode, so the menu bar and Dock hide while it is up. This
// works the same inside a host (Logic) and in the standalone app, because
// the plugin's own editor window is left alone.
//
// Overlay:
//   - A translucent control panel along the bottom with every parameter.
//     Toggle it with the Hide/Show controls button or C. Its state is saved
//     with the plugin, so full screen reopens the way you left it.
//   - Top-right buttons (Controls, Exit) that appear when the mouse moves
//     and hide after two seconds. With the controls hidden, the cursor
//     hides too.
// Exit: Esc, F, double-click on the scope, or the Exit button.
class FullScreenWindow : public juce::Component, private juce::Timer
{
public:
    FullScreenWindow (CircleScopeProcessor& p, juce::Rectangle<int> displayArea, std::function<void()> onExit)
        : processor (p), scope (p), controls (p, nullptr, true), exitCallback (std::move (onExit))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        addAndMakeVisible (scope);

        // Overlay components live inside the scope: its OpenGL view would
        // cover siblings, but its own children are painted into the GL frame.
        exitButton.onClick = [this] { requestExit(); };
        controlsButton.onClick = [this] { setControlsVisible (! controls.isVisible()); };
        hint.setText ("Esc / F / double-click: exit    C: controls", juce::dontSendNotification);
        hint.setJustificationType (juce::Justification::centredRight);
        hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
        for (juce::Component* c : std::initializer_list<juce::Component*> { &exitButton, &controlsButton, &hint })
            scope.addAndMakeVisible (c);
        scope.addChildComponent (controls);

        scope.onDoubleClick = [this] { requestExit(); };
        scope.addMouseListener (this, true);

        setControlsVisible (processor.apvts.state.getProperty (controlsVisibleId, true));

        setBounds (displayArea);
        addToDesktop (0);
        setVisible (true);
        juce::Desktop::getInstance().setKioskModeComponent (this, false);
        toFront (true);
        grabKeyboardFocus();
        showOverlay();
    }

    ~FullScreenWindow() override
    {
        stopTimer();
        scope.removeMouseListener (this);
        if (juce::Desktop::getInstance().getKioskModeComponent() == this)
            juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    void resized() override
    {
        scope.setBounds (getLocalBounds());

        auto top = getLocalBounds().reduced (16).removeFromTop (30);
        exitButton.setBounds (top.removeFromRight (150));
        top.removeFromRight (8);
        controlsButton.setBounds (top.removeFromRight (130));
        hint.setBounds (getWidth() - 416, 50, 400, 20);

        const int panelWidth = juce::jmin (getWidth() - 32, 1100);
        controls.setBounds ((getWidth() - panelWidth) / 2, getHeight() - ControlPanel::preferredHeight - 16,
                            panelWidth, ControlPanel::preferredHeight);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        if (key == juce::KeyPress::escapeKey || c == 'f')
        {
            requestExit();
            return true;
        }
        if (c == 'c')
        {
            setControlsVisible (! controls.isVisible());
            return true;
        }
        return false;
    }

    void mouseMove (const juce::MouseEvent&) override { showOverlay(); }

private:
    static inline const juce::Identifier controlsVisibleId { "fullScreenControls" };

    void setControlsVisible (bool shouldShow)
    {
        controls.setVisible (shouldShow);
        controlsButton.setButtonText (shouldShow ? "Hide controls" : "Show controls");
        processor.apvts.state.setProperty (controlsVisibleId, shouldShow, nullptr);
        showOverlay();
    }

    void showOverlay()
    {
        exitButton.setVisible (true);
        controlsButton.setVisible (true);
        hint.setVisible (true);
        scope.setMouseCursor (juce::MouseCursor::NormalCursor);
        lastActivityMs = juce::Time::getMillisecondCounter();
        startTimer (250);
    }

    void timerCallback() override
    {
        if (juce::Time::getMillisecondCounter() - lastActivityMs < 2000)
            return;

        exitButton.setVisible (false);
        controlsButton.setVisible (false);
        hint.setVisible (false);
        if (! controls.isVisible())
            scope.setMouseCursor (juce::MouseCursor::NoCursor);
        stopTimer();
    }

    // The owner deletes this window, so never do that from inside our own
    // callback stack: defer to the message loop.
    void requestExit()
    {
        if (exiting)
            return;
        exiting = true;
        juce::MessageManager::callAsync ([cb = exitCallback] { if (cb) cb(); });
    }

    CircleScopeProcessor& processor;
    ScopeComponent scope;
    ControlPanel controls;
    juce::TextButton exitButton { "Exit full screen" }, controlsButton { "Hide controls" };
    juce::Label hint;
    std::function<void()> exitCallback;
    juce::uint32 lastActivityMs = 0;
    bool exiting = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullScreenWindow)
};
