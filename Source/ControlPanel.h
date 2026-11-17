#pragma once

#include "PluginProcessor.h"

// Where the Field EQ's curve is drawn: distance from the instance's
// position runs to the right, pitch upward, and the line says which pitch
// belongs at which distance. Drag to draw it.
class FieldCurveEditor : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit FieldCurveEditor (ScopeSource& sourceToEdit) : source (sourceToEdit) {}

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        const float dim = isEnabled() ? 1.0f : 0.4f;
        g.setColour (juce::Colour (0xff0d0d10));
        g.fillRoundedRectangle (area, 4.0f);

        // An octave apart, 20 Hz at the bottom.
        g.setColour (juce::Colours::white.withAlpha (0.07f * dim));
        const float octaves = (float) scope::fieldBands * scope::fieldBandOctaves;
        for (int octave = 1; (float) octave < octaves; ++octave)
            g.fillRect (area.getX(), area.getBottom() - area.getHeight() * (float) octave / octaves, area.getWidth(), 1.0f);

        juce::Path line;
        for (int i = 0; i < scope::fieldCurvePoints; ++i)
        {
            const auto p = pointFor (i);
            if (i == 0)
                line.startNewSubPath (p);
            else
                line.lineTo (p);
        }
        g.setColour (juce::Colour (0xff7f77dd).withAlpha (dim));
        g.strokePath (line, juce::PathStrokeType (2.0f));

        g.setColour (juce::Colours::white.withAlpha (0.4f * dim));
        g.setFont (11.0f);
        const auto text = getLocalBounds().reduced (5, 2);
        g.drawText ("20 kHz", text, juce::Justification::topLeft);
        g.drawText ("20 Hz, at the centre", text, juce::Justification::bottomLeft);
        g.drawText ("far out", text, juce::Justification::bottomRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        lastIndex = -1;
        mouseDrag (e);
    }

    // Sets the point under the mouse, and the ones it passed over since the last time.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int last = scope::fieldCurvePoints - 1;
        const int index = juce::jlimit (0, last, juce::roundToInt ((float) e.x / (float) juce::jmax (1, getWidth()) * (float) last));
        const float value = juce::jlimit (0.0f, 1.0f, 1.0f - (float) e.y / (float) juce::jmax (1, getHeight()));

        if (lastIndex < 0 || lastIndex == index)
        {
            source.fieldCurve[index].store (value);
        }
        else
        {
            const int step = index > lastIndex ? 1 : -1;
            for (int i = lastIndex + step; i != index + step; i += step)
            {
                const float t = (float) (i - lastIndex) / (float) (index - lastIndex);
                source.fieldCurve[i].store (lastValue + (value - lastValue) * t);
            }
        }

        lastIndex = index;
        lastValue = value;
        repaint();
    }

private:
    juce::Point<float> pointFor (int i) const
    {
        return { (float) getWidth() * (float) i / (float) (scope::fieldCurvePoints - 1),
                 (float) getHeight() * (1.0f - source.fieldCurve[i].load()) };
    }

    ScopeSource& source;
    int lastIndex = -1;
    float lastValue = 0.0f;
};

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
        planeBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pPlane].choices, "|", ""), 1);
        planeBox.setTooltip ("The plane this instance draws on, in the 3D scene it shares with the other layers of its stream. XY faces you; YZ is the side wall and XZ the floor, so turn the view to see them");
        planeLabel.setText ("Plane", juce::dontSendNotification);
        sumButton.setTooltip ("Add other CircleScope instances in this session to this one's drawing: at every moment each gives a point on its own plane, and the sum of them is the point drawn. This instance then shows, and streams, the whole 3D drawing");
        sumButton.onClick = [this] { showSumMenu(); };
        fieldLockButton.setTooltip ("Field mode: take the picture at the same point of the wave every frame, like a stroboscope, so the dots pulse slowly (Field speed) instead of flickering");
        colourBox.addItemList ({ "Frequency hue", "Fixed" }, 1);
        layoutBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pLayout].choices, "|", ""), 1);

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
                                                                            &planeLabel, &planeBox, &sumButton, &fieldLockButton, &fieldVolumeButton, &fieldShapeBox, &fieldEqButton, &fieldCurve,
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
        setUp (planeRotX, "planerotx", "Turn X", " deg");
        setUp (planeRotY, "planeroty", "Turn Y", " deg");
        setUp (planeRotZ, "planerotz", "Turn Z", " deg");
        setUp (viewYaw, "viewyaw", "View yaw", " deg");
        setUp (viewPitch, "viewpitch", "View pitch", " deg");
        setUp (viewRoll, "viewroll", "View roll", " deg");
        setUp (posX, "posx", "Position X", "");
        setUp (posY, "posy", "Position Y", "");
        setUp (posZ, "posz", "Position Z", "");
        setUp (fieldHue, "fieldhue", "Field hue", " deg");
        setUp (fieldFade, "fieldfade", "Field fade", " %");
        setUp (fieldSpeed, "fieldspeed", "Field speed", " %");
        setUp (fieldDensity, "fielddensity", "Density", " %");
        fieldDensity.slider.setTooltip ("Field mode, as a volume: how solid the dots are. Low, you see deep into the field; high, the nearest dots hide the ones behind");
        fieldShapeBox.addItemList (juce::StringArray::fromTokens (scope::paramTable[scope::pFieldShape].choices, "|", ""), 1);
        fieldShapeBox.setTooltip ("Field mode: Dots stand still and pulse, large for low notes and fine for high ones. Rings spread out from this instance's position, like ripples");
        setUp (fieldFocus, "fieldfocus", "Focus", "");
        setUp (fieldAperture, "fieldaperture", "Aperture", " %");
        fieldFocus.slider.setTooltip ("Field mode, as a volume: the depth that is sharp when Aperture is open. 0 is the centre of the scene, positive is nearer to you, negative further away");
        fieldFocus.slider.setDoubleClickReturnValue (true, 0.0);
        fieldAperture.slider.setTooltip ("Field mode, as a volume: depth of field, like opening a camera's lens. What is at the Focus depth stays sharp, and the rest blurs more the further it is from there. 0 is everything sharp");
        setUp (fieldEqWidth, "fieldeqwidth", "EQ width", " oct");
        fieldEqWidth.slider.setTooltip ("Field EQ: how wide the range of pitch is that each distance lets through, in octaves. Narrow tunes in to one pitch's pattern; at 10 everything passes");
        fieldEqButton.setTooltip ("Field mode: let only a range of pitch into the field at each distance from this instance's position, by the curve drawn below. So low notes can sit in the centre and high ones outside, or the other way round");
        fieldCurve.setTooltip ("Field EQ: drag to draw which pitch belongs at which distance from this instance's position. Left is the centre, right is two plane units out and beyond; bottom is 20 Hz, top 20 kHz");
        fieldVolumeButton.setTooltip ("Field mode: show the field as a volume of spheres in space, looked through from the camera. Off: only where the planes cut it, which is lighter on the graphics card. Turn the view to see the depth");
        fieldSpeed.slider.setTooltip ("Field mode, locked: how fast the dots pulse, in percent of real time. At 1 % a 100 Hz tone pulses once a second; at 0 the picture stands still");
        for (auto* ls : { &posX, &posY, &posZ })
        {
            ls->slider.setTooltip ("Moves the centre of this instance's plane in the scene: x to the right, y up, z towards you. 1 is the edge of the picture's square. In Field mode it is the centre of this instance's field");
            ls->slider.setDoubleClickReturnValue (true, 0.0);
        }
        fieldHue.slider.setTooltip ("Field mode: the colour of the field where it is positive. Where it is negative it gets the complementary colour");
        fieldFade.slider.setTooltip ("Field mode: how fast this instance's field fades with distance from its position. At 100 % it is gone at the edge of the picture's square");
        for (auto* ls : { &planeRotX, &planeRotY, &planeRotZ })
            ls->slider.setTooltip ("Turns this instance's plane about the scene's axis, on top of the Plane chosen");
        for (auto* ls : { &viewYaw, &viewPitch, &viewRoll })
            ls->slider.setTooltip ("Turns the whole drawing, summed planes and all. On a wall or cluster node, set it on one instance: the lowest layer that has turned its view sets it for all");
        for (auto* ls : { &planeRotX, &planeRotY, &planeRotZ, &viewYaw, &viewPitch, &viewRoll })
            ls->slider.setDoubleClickReturnValue (true, 0.0);

        modeAttachment = std::make_unique<ComboAttachment> (apvts, "mode", modeBox);
        planeAttachment = std::make_unique<ComboAttachment> (apvts, "plane", planeBox);
        colourAttachment = std::make_unique<ComboAttachment> (apvts, "colour", colourBox);
        layoutAttachment = std::make_unique<ComboAttachment> (apvts, "layout", layoutBox);
        swingMotionAttachment = std::make_unique<ComboAttachment> (apvts, "swingmotion", swingMotionBox);
        swingSyncAttachment = std::make_unique<ButtonAttachment> (apvts, "swingsync", swingSyncButton);
        fieldLockAttachment = std::make_unique<ButtonAttachment> (apvts, "fieldlock", fieldLockButton);
        fieldVolumeAttachment = std::make_unique<ButtonAttachment> (apvts, "fieldvolume", fieldVolumeButton);
        fieldShapeAttachment = std::make_unique<ComboAttachment> (apvts, "fieldshape", fieldShapeBox);
        fieldEqAttachment = std::make_unique<ButtonAttachment> (apvts, "fieldeq", fieldEqButton);
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

    static constexpr int preferredHeight = 496;

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

        // Rows 6 and 7: this instance's plane in the 3D scene, and the camera.
        auto row6 = controls.removeFromTop (30).reduced (0, 3);
        auto plane = row6.removeFromLeft (row6.getWidth() / 3);
        planeLabel.setBounds (plane.removeFromLeft (76));
        planeBox.setBounds (plane.reduced (4, 0));
        placeRow (row6.withWidth (row6.getWidth() * 3 / 2), { &planeRotX, &planeRotY });
        auto row7 = controls.removeFromTop (30).reduced (0, 3);
        placeRow (row7, { &planeRotZ });
        sumButton.setBounds (row7.withTrimmedLeft (row7.getWidth() / 3).removeFromLeft (150).reduced (4, 0));


        // Row 8: where the plane's centre is. Row 9: the Field mode.
        placeRow (controls.removeFromTop (30).reduced (0, 3), { &posX, &posY, &posZ });
        placeRow (controls.removeFromTop (30).reduced (0, 3), { &fieldHue, &fieldFade, &fieldSpeed });
        auto row10 = controls.removeFromTop (30).reduced (0, 3);
        placeRow (row10, { &fieldDensity });
        auto shape = row10.withTrimmedLeft (row10.getWidth() / 3).withWidth (row10.getWidth() / 3);
        fieldVolumeButton.setBounds (shape.removeFromLeft (90).reduced (4, 0));
        fieldShapeBox.setBounds (shape.removeFromLeft (120).reduced (4, 0));
        fieldLockButton.setBounds (row10.withTrimmedLeft (row10.getWidth() * 2 / 3).removeFromLeft (150).reduced (4, 0));

        placeRow (controls.removeFromTop (30).reduced (0, 3), { &fieldFocus, &fieldAperture });

        // Then the Field EQ, and its curve.
        auto row11 = controls.removeFromTop (30).reduced (0, 3);
        fieldEqButton.setBounds (row11.withWidth (row11.getWidth() / 3).reduced (4, 0));
        placeRow (row11.withTrimmedLeft (row11.getWidth() / 3).withWidth (row11.getWidth()), { &fieldEqWidth });
        fieldCurve.setBounds (controls.removeFromTop (60).reduced (4, 3));

        // Row 10: the camera, which turns the whole drawing.
        placeRow (controls.removeFromTop (30).reduced (0, 3), { &viewYaw, &viewPitch, &viewRoll });

        // Row 11: the streams this instance is sent to.
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

        // Likewise the field controls in Field mode, where Delay and Colour do nothing.
        const bool field = modeBox.getSelectedId() == (int) scope::Mode::field + 1;
        fieldHue.slider.setEnabled (field);
        fieldFade.slider.setEnabled (field);
        fieldLockButton.setEnabled (field);
        fieldSpeed.slider.setEnabled (field && fieldLockButton.getToggleState());
        fieldVolumeButton.setEnabled (field);
        fieldShapeBox.setEnabled (field);
        fieldEqButton.setEnabled (field);
        fieldEqWidth.slider.setEnabled (field && fieldEqButton.getToggleState());
        fieldCurve.setEnabled (field && fieldEqButton.getToggleState());
        fieldDensity.slider.setEnabled (field && fieldVolumeButton.getToggleState());
        fieldAperture.slider.setEnabled (field && fieldVolumeButton.getToggleState());
        fieldFocus.slider.setEnabled (field && fieldVolumeButton.getToggleState() && fieldAperture.slider.getValue() > 0.0);

        // On a mono track there is one picture, whatever the layout says.
        const bool mono = processor.source.mono.load();
        layoutBox.setEnabled (! mono);
        layoutBox.setTooltip (mono ? "This track is mono, so it is drawn as one picture" : "");

        const auto partners = processor.getPartners().size();
        sumButton.setButtonText (partners == 0 ? juce::String ("Sum with...") : "Sum with " + juce::String (partners));

        bool sending = false;
        for (auto& button : streamButtons)
            sending = sending || button.getToggleState();
        const int nodes = processor.getNumNetworkTargets();
        streamStatus.setText (! sending ? juce::String()
                                        : nodes == 0 ? juce::String ("nothing listening")
                                                     : juce::String (nodes) + (nodes == 1 ? " receiver" : " receivers"),
                              juce::dontSendNotification);
    }

    // The other instances in the host, to tick the ones whose audio is
    // added to this instance's drawing.
    void showSumMenu()
    {
        const auto chosen = processor.getPartnerIds();
        auto isChosen = [&chosen] (const juce::Uuid& id) { return std::find (chosen.begin(), chosen.end(), id) != chosen.end(); };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Add to this instance's drawing");
        std::vector<std::pair<juce::Uuid, bool>> items;
        for (const auto& entry : InstanceRegistry::get().list())
        {
            if (entry.id == processor.getInstanceId())
                continue;
            items.push_back ({ entry.id, isChosen (entry.id) });
            menu.addItem ((int) items.size(), entry.displayName(), true, items.back().second);
        }
        if (items.empty())
            menu.addItem (1000, "No other CircleScope instances in this host", false, false);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sumButton),
                            [safe = juce::Component::SafePointer<ControlPanel> (this), items] (int result)
                            {
                                if (safe != nullptr && result >= 1 && result <= (int) items.size())
                                    safe->processor.setPartner (items[(size_t) result - 1].first, ! items[(size_t) result - 1].second);
                            });
    }

    CircleScopeProcessor& processor;
    juce::Component* trailingComponent;
    const bool isTranslucent;

    juce::ComboBox modeBox, colourBox, layoutBox, blendBox, layerBox, swingMotionBox, planeBox, fieldShapeBox;
    juce::ToggleButton guidesButton { "Guides" }, testButton { "Test tone" }, divideButton { "Divide" },
                       swingSyncButton { "Once per window" }, fieldLockButton { "Lock to wave" }, fieldVolumeButton { "Volume" }, fieldEqButton { "EQ by distance" };
    FieldCurveEditor fieldCurve { processor.source };
    juce::TextButton sumButton { "Sum with..." };
    juce::Label blendLabel, layerLabel, streamLabel, streamStatus, swingLabel, planeLabel;
    std::array<juce::TextButton, net::maxStreams> streamButtons;

    // Row 2: signal controls. Row 3: image controls. Row 4: band limits, layering. Row 5: swing.
    // Rows 6 and 7: plane, sum. Row 8: position. Rows 9 to 13: field. Row 14: camera. Row 15: streams.
    LabelledSlider window, delay, afterglow, brightness, contrast, blur, lowCut, highCut, swingRate,
                   planeRotX, planeRotY, planeRotZ, viewYaw, viewPitch, viewRoll,
                   posX, posY, posZ, fieldHue, fieldFade, fieldSpeed, fieldDensity, fieldEqWidth, fieldFocus, fieldAperture;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment, blendAttachment, layerAttachment, swingMotionAttachment, planeAttachment, fieldShapeAttachment;
    std::unique_ptr<ButtonAttachment> guidesAttachment, testAttachment, divideAttachment, swingSyncAttachment, fieldLockAttachment, fieldVolumeAttachment, fieldEqAttachment;
    std::array<std::unique_ptr<ButtonAttachment>, net::maxStreams> streamAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};
