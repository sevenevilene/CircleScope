#pragma once

#include "ScopeComponent.h"
#include "PluginProcessor.h"

// Borderless top-level window that fills one display with its own scope and
// nothing else: no controls, no buttons, no labels, no cursor. It uses kiosk
// mode, so the menu bar and Dock hide while it is up. This works the same
// inside a host (Logic) and in the standalone app, because the plugin's own
// editor window is left alone.
//
// Keys:
//   Esc / F   exit (so does a double-click)
//   M         mode            L   layout          C   colour
//   G         guides on/off   T   test tone
// Everything else is driven from the editor, host automation or MIDI.
class FullScreenWindow : public juce::Component
{
public:
    FullScreenWindow (CircleScopeProcessor& p, juce::Rectangle<int> displayArea, std::function<void()> onExit)
        : processor (p), scope (p.source, false), exitCallback (std::move (onExit))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        addAndMakeVisible (scope);

        scope.onDoubleClick = [this] { requestExit(); };
        scope.setMouseCursor (juce::MouseCursor::NoCursor);
        setMouseCursor (juce::MouseCursor::NoCursor);

        setBounds (displayArea);
        addToDesktop (0);
        setVisible (true);
        juce::Desktop::getInstance().setKioskModeComponent (this, false);
        toFront (true);
        grabKeyboardFocus();
    }

    ~FullScreenWindow() override
    {
        if (juce::Desktop::getInstance().getKioskModeComponent() == this)
            juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    void resized() override { scope.setBounds (getLocalBounds()); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            requestExit();
            return true;
        }

        switch (juce::CharacterFunctions::toLowerCase (key.getTextCharacter()))
        {
            case 'f': requestExit(); return true;
            case 'm': processor.cycleParameter (scope::pMode); return true;
            case 'l': processor.cycleParameter (scope::pLayout); return true;
            case 'c': processor.cycleParameter (scope::pColour); return true;
            case 'g': processor.cycleParameter (scope::pGuides); return true;
            case 't': processor.cycleParameter (scope::pTest); return true;
            default:  return false;
        }
    }

private:
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
    std::function<void()> exitCallback;
    bool exiting = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullScreenWindow)
};
