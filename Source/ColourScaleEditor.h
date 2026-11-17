#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "ColourScale.h"

// Where the scene's colour scale is set (ColourScale.h): a strip with
// 20 Hz at the left and 20 kHz at the right, and a dot at every stop.
//
//   click the strip           a new stop there
//   drag a stop               moves it; dragged well off the strip, it goes
//   double-click a stop       its colour
//   right-click a stop        removes it
//
// The scale is the scene's, so every instance's editor shows the same one,
// and follows when another changes it.
class ColourScaleEditor : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::Timer
{
public:
    ColourScaleEditor()
    {
        setTooltip ("The scene's colours by pitch, 20 Hz at the left to 20 kHz at the right, shared by every instance. Click to add a stop, drag to move it, double-click it for its colour, right-click to remove it. In Field mode this is the colour at full positive amplitude; negative gets the opposite hue");
        reload();
        startTimerHz (8);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bar = getBar();
        g.reduceClipRegion (getLocalBounds());

        for (int x = 0; x < (int) bar.getWidth(); ++x)
        {
            g.setColour (colourAt ((float) x / juce::jmax (1.0f, bar.getWidth() - 1.0f)));
            g.fillRect (bar.getX() + (float) x, bar.getY(), 1.0f, bar.getHeight());
        }

        for (int i = 0; i < scale.count; ++i)
        {
            const auto& s = scale.stops[i];
            const auto dot = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ bar.getX() + s.at * bar.getWidth(), bar.getCentreY() });
            g.setColour (juce::Colour::fromFloatRGBA (s.r, s.g, s.b, 1.0f));
            g.fillEllipse (dot);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.drawEllipse (dot, 2.5f);
            g.setColour (juce::Colours::white.withAlpha (i == selected ? 1.0f : 0.75f));
            g.drawEllipse (dot, 1.2f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        reload();
        selected = stopAt (e.position);
        dragging = false;

        if (e.mods.isPopupMenu())
        {
            removeSelected();
            return;
        }

        if (selected < 0 && scale.count < scope::maxColourStops)
        {
            const float at = placeAt (e.position.x);
            scope::ColourStop stop { at };
            float rgb[3];
            scale.colourAt (at, rgb);
            stop.r = rgb[0];
            stop.g = rgb[1];
            stop.b = rgb[2];
            scale.stops[scale.count++] = stop;
            keepSorted (stop);
            store();
        }
        dragging = selected >= 0;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || selected < 0 || ! e.mouseWasDraggedSinceMouseDown())
            return;

        // Pulled well clear of the strip: the stop is taken out.
        if (std::abs (e.position.y - (float) getHeight() * 0.5f) > 40.0f && scale.count > 2)
        {
            removeSelected();
            dragging = false;
            return;
        }

        auto stop = scale.stops[selected];
        stop.at = placeAt (e.position.x);
        scale.stops[selected] = stop;
        keepSorted (stop);
        store();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        selected = stopAt (e.position);
        if (selected < 0)
            return;

        const auto& s = scale.stops[selected];
        auto picker = std::make_unique<Picker> (*this);
        picker->setCurrentColour (juce::Colour::fromFloatRGBA (s.r, s.g, s.b, 1.0f), juce::dontSendNotification);
        picker->setSize (240, 280);
        picker->addChangeListener (picker.get());
        juce::CallOutBox::launchAsynchronously (std::move (picker), getScreenBounds(), nullptr);
    }

private:
    // The colour chooser of the selected stop.
    struct Picker : juce::ColourSelector,
                    juce::ChangeListener
    {
        explicit Picker (ColourScaleEditor& e)
            : juce::ColourSelector (juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders | juce::ColourSelector::showColourspace),
              editor (&e)
        {
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (editor != nullptr)
                editor->setSelectedColour (getCurrentColour());
        }

        juce::Component::SafePointer<ColourScaleEditor> editor;
    };

    juce::Rectangle<float> getBar() const { return getLocalBounds().toFloat().reduced (8.0f, 4.0f); }

    float placeAt (float x) const
    {
        const auto bar = getBar();
        return juce::jlimit (0.0f, 1.0f, (x - bar.getX()) / juce::jmax (1.0f, bar.getWidth()));
    }

    int stopAt (juce::Point<float> p) const
    {
        const auto bar = getBar();
        int best = -1;
        float nearest = 8.0f;
        for (int i = 0; i < scale.count; ++i)
        {
            const float d = std::abs (p.x - (bar.getX() + scale.stops[i].at * bar.getWidth()));
            if (d < nearest)
            {
                nearest = d;
                best = i;
            }
        }
        return best;
    }

    juce::Colour colourAt (float at) const
    {
        float rgb[3];
        scale.colourAt (at, rgb);
        return juce::Colour::fromFloatRGBA (rgb[0], rgb[1], rgb[2], 1.0f);
    }

    // Puts the stops in order again, and keeps hold of the one given.
    void keepSorted (const scope::ColourStop& held)
    {
        scale.sort();
        for (int i = 0; i < scale.count; ++i)
            if (juce::exactlyEqual (scale.stops[i].at, held.at) && juce::exactlyEqual (scale.stops[i].r, held.r)
                && juce::exactlyEqual (scale.stops[i].g, held.g) && juce::exactlyEqual (scale.stops[i].b, held.b))
                selected = i;
    }

    void removeSelected()
    {
        if (selected < 0 || scale.count <= 2)
            return;
        for (int i = selected; i + 1 < scale.count; ++i)
            scale.stops[i] = scale.stops[i + 1];
        --scale.count;
        selected = -1;
        store();
    }

    void setSelectedColour (juce::Colour c)
    {
        if (selected < 0 || selected >= scale.count)
            return;
        auto& s = scale.stops[selected];
        s.r = c.getFloatRed();
        s.g = c.getFloatGreen();
        s.b = c.getFloatBlue();
        store();
    }

    void store()
    {
        auto& scene = scope::SceneColours::get();
        scene.write (scale, juce::Time::currentTimeMillis());
        shownVersion = scene.getVersion();
        repaint();
    }

    void reload()
    {
        auto& scene = scope::SceneColours::get();
        shownVersion = scene.getVersion();
        scale = scene.read();
        selected = juce::jmin (selected, scale.count - 1);
    }

    void timerCallback() override
    {
        if (! dragging && scope::SceneColours::get().getVersion() != shownVersion)
        {
            reload();
            repaint();
        }
    }

    scope::ColourScale scale;
    juce::uint32 shownVersion = 0;
    int selected = -1;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColourScaleEditor)
};
