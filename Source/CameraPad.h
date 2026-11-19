#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Plane.h"

// The camera's controls, in a corner of the scope. Two sticks and six
// arrows, each of which works for as long as it is held:
//
//   Orbit  the camera goes round the point it looks at (View yaw / pitch)
//   Look   the camera turns where it stands, like turning your head: the
//          point it looks at swings round it (View yaw / pitch and
//          Camera X / Y / Z together)
//   Move   the camera travels: right, left, up, down, forward and back as
//          it sees them (Camera X / Y / Z)
//
// So holding Forward and steering with Look flies through the scene. It
// all goes through the plugin's parameters, as gestures, so a host can
// record a flight as automation and play it back. Reset (or a double-click
// on the pad) puts the camera back where it started: straight on, at the
// centre, with no roll. View has the sides to look from: Front, Back, Top,
// Bottom, Left and Right turn the camera there (View yaw / pitch, no roll),
// about the point it looks at. The same menu has the stereo setting of this
// instance's own scopes (see Plane.h): which eyes, and how deep.
//
// It only shows while the mouse is over the scope.
class CameraPad : public juce::Component,
                  public juce::SettableTooltipClient,
                  private juce::Timer
{
public:
    static constexpr int width = 306, height = 100;

    explicit CameraPad (juce::AudioProcessorValueTreeState& state)
    {
        const char* ids[numValues] { "viewyaw", "viewpitch", "camx", "camy", "camz" };
        for (int i = 0; i < numValues; ++i)
            params[i] = state.getParameter (ids[i]);
        roll = state.getRawParameterValue ("viewroll");
        rollParam = state.getParameter ("viewroll");

        setSize (width, height);
        setTooltip ("Camera. Orbit: go round the point it looks at. Look: turn it where it stands. Move: hold an arrow to travel; hold the one pointing away and steer with Look to fly through. View looks from a side: front, back, top, bottom, left or right, and has the stereo setting. Reset puts the view back");
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        if (! shown)
            return;

        const auto line = juce::Colours::white.withAlpha (0.55f);
        const auto solid = juce::Colours::white.withAlpha (0.75f);

        for (int i = 0; i < 2; ++i)
        {
            const auto centre = stickCentre (i);
            const auto ring = juce::Rectangle<float> (stickRadius * 2.0f, stickRadius * 2.0f).withCentre (centre);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillEllipse (ring);
            g.setColour (line);
            g.drawEllipse (ring, 1.0f);

            const auto& v = i == 0 ? orbit : look;
            g.setColour (held == i ? juce::Colours::white : solid);
            g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (centre + juce::Point<float> (v.x, -v.y) * stickThrow));
        }

        for (int i = 0; i < numArrows; ++i)
        {
            const auto dir = arrowDirection (i);
            const auto tip = arrowTip (i);
            const float size = i == forward ? 6.0f : 8.0f;
            const auto unit = dir / dir.getDistanceFromOrigin();
            const juce::Point<float> across (-unit.y, unit.x);

            g.setColour (line);
            g.drawLine ({ arrowCentre + dir * 6.0f, tip }, 1.0f);

            juce::Path head;
            head.addTriangle (tip + unit * (size * 1.3f), tip + across * (size * 0.8f), tip - across * (size * 0.8f));
            g.setColour (held == firstArrow + i ? juce::Colours::white : solid);
            g.fillPath (head);
        }

        g.setFont (12.0f);
        for (const auto& [area, name] : { std::pair { viewArea, "View" }, std::pair { resetArea, "Reset" } })
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (area, 6.0f);
            g.setColour (line);
            g.drawRoundedRectangle (area, 6.0f, 1.0f);
            g.setColour (solid);
            g.drawText (name, area, juce::Justification::centred);
        }

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (11.0f);
        const char* names[] { "Orbit", "Look", "Move" };
        const float centres[] { stickCentre (0).x, stickCentre (1).x, arrowCentre.x };
        for (int i = 0; i < 3; ++i)
            g.drawText (names[i], juce::Rectangle<float> (60.0f, 14.0f).withCentre ({ centres[i], 90.0f }), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (resetArea.contains (e.position))
        {
            resetCamera();
            return;
        }

        if (viewArea.contains (e.position))
        {
            showViewMenu();
            return;
        }

        held = controlAt (e.position);
        if (held < 0)
            return;

        // From here on the camera is worked out here and written to the
        // parameters, so nothing is lost to their rounding.
        for (int i = 0; i < numValues; ++i)
        {
            value[i] = params[i] != nullptr ? (double) params[i]->convertFrom0to1 (params[i]->getValue()) : 0.0;
            if (params[i] != nullptr)
                params[i]->beginChangeGesture();
        }

        lastMs = juce::Time::getMillisecondCounterHiRes();
        mouseDrag (e);
        startTimerHz (60);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (held != 0 && held != 1)
            return;

        auto v = (e.position - stickCentre (held)) / stickThrow;
        const float length = v.getDistanceFromOrigin();
        if (length > 1.0f)
            v /= length;
        (held == 0 ? orbit : look) = { v.x, -v.y };
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (held < 0)
            return;

        for (auto* p : params)
            if (p != nullptr)
                p->endChangeGesture();

        held = -1;
        orbit = look = {};
        startTimerHz (10);
        repaint();
    }

    // Where this instance keeps the stereo setting of its scopes: a
    // scope::Stereo, a depth in percent and Wiggle's changes of eye a
    // second. They must outlive the pad.
    void setStereoSettings (std::atomic<int>* mode, std::atomic<int>* depth, std::atomic<int>* rate)
    {
        stereoRate = rate;
        stereoMode = mode;
        stereoDepth = depth;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! viewArea.contains (e.position))
            resetCamera();
    }

    // Look at the scene from a side: the camera goes there about the point
    // it looks at, which stays, and loses its roll.
    void lookFrom (scope::Perspective from)
    {
        const auto angles = scope::perspectiveAngles (from);
        setParam (params[yaw], angles.yaw);
        setParam (params[pitch], angles.pitch);
        setParam (rollParam, 0.0f);
    }

    // Straight on, at the centre of the scene, with no roll.
    void resetCamera()
    {
        auto toDefault = [] (juce::RangedAudioParameter* p)
        {
            if (p == nullptr)
                return;
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        };

        for (auto* p : params)
            toDefault (p);
        toDefault (rollParam);
    }

private:
    static void setParam (juce::RangedAudioParameter* p, float value)
    {
        if (p == nullptr)
            return;
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }

    // The sides, with the one the camera is at ticked, and the stereo setting.
    void showViewMenu()
    {
        auto now = [] (juce::RangedAudioParameter* p) { return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f; };
        const float yawNow = now (params[yaw]), pitchNow = now (params[pitch]);
        const bool level = std::abs (now (rollParam)) < 0.05f;

        juce::PopupMenu menu;
        for (int i = 0; i < scope::numPerspectives; ++i)
        {
            const auto angles = scope::perspectiveAngles ((scope::Perspective) i);
            const bool there = level && std::abs (pitchNow - angles.pitch) < 0.05f
                                     && std::abs (std::remainder (yawNow - angles.yaw, 360.0f)) < 0.05f;
            menu.addItem (i + 1, scope::perspectiveNames[i], true, there);
        }

        if (stereoMode != nullptr && stereoDepth != nullptr && stereoRate != nullptr)
        {
            menu.addSeparator();
            menu.addSectionHeader ("Stereo");
            for (int i = 0; i < scope::numStereoModes; ++i)
                menu.addItem (firstStereoId + i, scope::stereoNames[i], true, stereoMode->load() == i);

            juce::PopupMenu depths;
            for (int percent : scope::stereoDepths)
                depths.addItem (firstDepthId + percent, juce::String (percent) + "%", true, stereoDepth->load() == percent);
            menu.addSubMenu ("Depth", depths);

            juce::PopupMenu rates;
            for (int perSecond : scope::wiggleRates)
                rates.addItem (firstRateId + perSecond, juce::String (perSecond) + " a second", true, stereoRate->load() == perSecond);
            menu.addSubMenu ("Wiggle rate", rates);
        }

        juce::Component::SafePointer<CameraPad> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                      .withTargetScreenArea (localAreaToGlobal (viewArea.toNearestInt())),
                            [safeThis] (int chosen)
                            {
                                if (safeThis == nullptr || chosen <= 0)
                                    return;

                                if (chosen >= firstRateId)
                                    safeThis->stereoRate->store (chosen - firstRateId);
                                else if (chosen >= firstDepthId)
                                    safeThis->stereoDepth->store (chosen - firstDepthId);
                                else if (chosen >= firstStereoId)
                                    safeThis->stereoMode->store (chosen - firstStereoId);
                                else
                                    safeThis->lookFrom ((scope::Perspective) (chosen - 1));
                            });
    }

    enum { yaw = 0, pitch, camX, camY, camZ, numValues };
    enum { right = 0, left, up, down, forward, back, numArrows };
    static constexpr int firstStereoId = 100, firstDepthId = 1000, firstRateId = 2000;   // in the View menu
    static constexpr int firstArrow = 2;                 // "held": 0 orbit, 1 look, then the arrows
    static constexpr float stickRadius = 32.0f, stickThrow = 23.0f;
    static constexpr float turnRate = 70.0f;             // degrees a second, with a stick right out
    static constexpr float moveRate = 1.5f;              // plane units a second
    static inline const juce::Point<float> arrowCentre { 204.0f, 42.0f };
    static inline const juce::Rectangle<float> viewArea { 252.0f, 14.0f, 48.0f, 24.0f };
    static inline const juce::Rectangle<float> resetArea { 252.0f, 46.0f, 48.0f, 24.0f };

    static juce::Point<float> stickCentre (int i) { return { i == 0 ? 44.0f : 124.0f, 42.0f }; }

    // On the pad, y down. Forward points away, up and to the right, and is drawn smaller.
    static juce::Point<float> arrowDirection (int i)
    {
        const juce::Point<float> directions[numArrows] { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f }, { 0.62f, -0.62f }, { -0.62f, 0.62f } };
        return directions[i];
    }

    static juce::Point<float> arrowTip (int i) { return arrowCentre + arrowDirection (i) * (i == forward ? 24.0f : 30.0f); }

    static int controlAt (juce::Point<float> p)
    {
        for (int i = 0; i < 2; ++i)
            if (p.getDistanceFrom (stickCentre (i)) <= stickRadius)
                return i;
        for (int i = 0; i < numArrows; ++i)
            if (p.getDistanceFrom (arrowTip (i) + arrowDirection (i) * 3.0f) <= 11.0f)
                return firstArrow + i;
        return -1;
    }

    void timerCallback() override
    {
        // Only there while the mouse is over the scope, or it is in use.
        const auto* parent = getParentComponent();
        const bool show = held >= 0 || (parent != nullptr && parent->isMouseOver (true));
        if (show != shown)
        {
            shown = show;
            repaint();
        }

        if (held < 0)
            return;

        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.0, 0.1, (now - lastMs) * 0.001);
        lastMs = now;

        const scope::Vec3 eye { 0.0f, 0.0f, scope::eyeDistance };
        const float rollNow = roll != nullptr ? roll->load() : 0.0f;
        auto camera = [&] { return scope::cameraOrientation ((float) value[yaw], (float) value[pitch], rollNow); };
        auto add = [this] (scope::Vec3 v, float k)
        {
            value[camX] += (double) (v.x * k);
            value[camY] += (double) (v.y * k);
            value[camZ] += (double) (v.z * k);
        };

        if (held == 0)
        {
            // Stick to the right: the camera goes round to the right.
            value[yaw] -= (double) (orbit.x * turnRate * dt);
            value[pitch] += (double) (orbit.y * turnRate * dt);
        }
        else if (held == 1)
        {
            // The eye stays where it is, and the point looked at swings round it.
            add (turnedBack (camera()) * eye, 1.0f);
            value[yaw] += (double) (look.x * turnRate * dt);
            value[pitch] -= (double) (look.y * turnRate * dt);
            value[pitch] = juce::jlimit (-90.0, 90.0, value[pitch]);
            add (turnedBack (camera()) * eye, -1.0f);
        }
        else
        {
            const scope::Vec3 ways[numArrows] { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
            add (turnedBack (camera()) * ways[held - firstArrow], moveRate * dt);
        }

        value[pitch] = juce::jlimit (-90.0, 90.0, value[pitch]);
        value[yaw] = std::remainder (value[yaw], 360.0);
        for (int i = camX; i <= camZ; ++i)
            value[i] = juce::jlimit (-20.0, 20.0, value[i]);

        for (int i = 0; i < numValues; ++i)
            if (params[i] != nullptr)
                params[i]->setValueNotifyingHost (params[i]->convertTo0to1 ((float) value[i]));
    }

    // The viewer's space back to the scene: the transpose.
    static scope::Mat3 turnedBack (const scope::Mat3& m)
    {
        scope::Mat3 r;
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                r.m[col * 3 + row] = m.m[row * 3 + col];
        return r;
    }

    juce::RangedAudioParameter* params[numValues] {};
    std::atomic<float>* roll = nullptr;
    std::atomic<int>* stereoMode = nullptr;
    std::atomic<int>* stereoDepth = nullptr;
    std::atomic<int>* stereoRate = nullptr;
    juce::RangedAudioParameter* rollParam = nullptr;
    double value[numValues] {};
    juce::Point<float> orbit, look;                      // the sticks: x right, y up, -1..1
    int held = -1;
    bool shown = false;
    double lastMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraPad)
};
