#pragma once

#include "PluginProcessor.h"
#include "ColourScaleEditor.h"

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
        coloursLabel.setText ("Colours", juce::dontSendNotification);
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
                                                                            &planeLabel, &planeBox, &sumButton, &fieldLockButton, &fieldVolumeButton, &fieldShapeBox, &fieldEqButton, &fieldCurve, &coloursLabel, &colourScale,
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
        setUp (fieldFade, "fieldfade", "Field fade", " %");
        setUp (fieldSpeed, "fieldspeed", "Field speed", " %");
        setUp (fieldDensity, "fielddensity", "Density", " %");
        fieldDensity.slider.setTooltip ("Field mode, as a volume: how solid the dots are. Low, you see deep into the field; high, the nearest dots hide the ones behind");
        setUp (alpha, "alpha", "Alpha", " %");
        alpha.slider.setTooltip ("The alpha of the colours, in every mode. For curves and the field on its planes it is how much of their light is added; in the field as a volume it is how much a dot hides what is behind it, without making it dimmer");
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

        modeBox.onChange = [this] { updateVisibility(); };
        fieldEqButton.onClick = [this] { updateVisibility(); };
        updateVisibility();

        startTimerHz (8);
        timerCallback();
    }

    // The panel only shows what the mode in use has a use for, so its
    // height goes by the mode. onHeightChanged says when that changed.
    static constexpr int rowHeight = 30, curveHeight = 60;

    int getPreferredHeight() const
    {
        return 12 + rowHeight * (9 + (showsSwing ? 1 : 0) + (showsField ? 4 : 0)) + (showsCurve ? curveHeight : 0);
    }

    std::function<void()> onHeightChanged;

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
        auto nextRow = [&controls] { return controls.removeFromTop (rowHeight).reduced (0, 3); };

        // Sliders in the thirds of a row, from the left.
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
        auto thirdOf = [] (juce::Rectangle<int> row, int index)
        {
            const int third = row.getWidth() / 3;
            return row.withX (row.getX() + index * third).withWidth (third);
        };
        auto fromSecondThird = [] (juce::Rectangle<int> row) { return row.withTrimmedLeft (row.getWidth() / 3).withWidth (row.getWidth()); };

        // Row 1: what is drawn, and the buttons.
        auto row1 = nextRow();
        for (auto* box : { &modeBox, &colourBox, &layoutBox })
        {
            if (! box->isVisible())
                continue;
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

        // Row 2: the signal. Row 3: the image.
        if (delay.slider.isVisible())
        {
            // Four in this row, so in quarters.
            auto row = nextRow();
            const int quarter = row.getWidth() / 4;
            for (auto* ls : { &window, &delay, &afterglow, &alpha })
            {
                auto cell = row.removeFromLeft (quarter);
                ls->label.setBounds (cell.removeFromLeft (76));
                ls->slider.setBounds (cell.reduced (4, 0));
            }
        }
        else
            placeRow (nextRow(), { &window, &afterglow, &alpha });
        placeRow (nextRow(), { &brightness, &contrast, &blur });

        // Row 4: band limits, then how this instance layers in the third cell.
        auto row4 = nextRow();
        placeRow (row4, { &lowCut, &highCut });
        auto layering = thirdOf (row4, 2);
        blendLabel.setBounds (layering.removeFromLeft (76));
        layerBox.setBounds (layering.removeFromRight (64).reduced (4, 0));
        layerLabel.setBounds (layering.removeFromRight (46));
        blendBox.setBounds (layering.reduced (4, 0));

        // The axis of the Swing mode.
        if (showsSwing)
        {
            auto row = nextRow();
            auto motion = thirdOf (row, 0);
            swingLabel.setBounds (motion.removeFromLeft (76));
            swingMotionBox.setBounds (motion.reduced (4, 0));
            placeRow (fromSecondThird (row), { &swingRate });
            swingSyncButton.setBounds (thirdOf (row, 2).removeFromLeft (150).reduced (4, 0));
        }

        // This instance's plane in the 3D scene, what is summed with it, and the scene's colours.
        auto planeRow = nextRow();
        auto plane = thirdOf (planeRow, 0);
        planeLabel.setBounds (plane.removeFromLeft (76));
        planeBox.setBounds (plane.reduced (4, 0));
        placeRow (fromSecondThird (planeRow), { &planeRotX, &planeRotY });

        auto turnRow = nextRow();
        placeRow (turnRow, { &planeRotZ });
        sumButton.setBounds (thirdOf (turnRow, 1).removeFromLeft (150).reduced (4, 0));
        auto colours = thirdOf (turnRow, 2);
        coloursLabel.setBounds (colours.removeFromLeft (76));
        colourScale.setBounds (colours.withTrimmedRight (4));

        // Where the plane's centre is.
        placeRow (nextRow(), { &posX, &posY, &posZ });

        // The Field mode.
        if (showsField)
        {
            auto fadeRow = nextRow();
            placeRow (fadeRow, { &fieldFade, &fieldSpeed });
            fieldLockButton.setBounds (thirdOf (fadeRow, 2).removeFromLeft (150).reduced (4, 0));

            auto volumeRow = nextRow();
            auto shape = thirdOf (volumeRow, 0);
            fieldShapeBox.setBounds (shape.removeFromLeft (120).reduced (4, 0));
            fieldVolumeButton.setBounds (shape.removeFromLeft (90).reduced (4, 0));
            placeRow (fromSecondThird (volumeRow), { &fieldDensity });

            placeRow (nextRow(), { &fieldFocus, &fieldAperture });

            // Then the Field EQ, and its curve.
            auto eqRow = nextRow();
            fieldEqButton.setBounds (thirdOf (eqRow, 0).reduced (4, 0));
            placeRow (fromSecondThird (eqRow), { &fieldEqWidth });
            if (showsCurve)
                fieldCurve.setBounds (controls.removeFromTop (curveHeight).reduced (4, 3));
        }

        // The camera, which turns the whole drawing.
        placeRow (nextRow(), { &viewYaw, &viewPitch, &viewRoll });

        // The streams this instance is sent to.
        auto streams = nextRow();
        streamLabel.setBounds (streams.removeFromLeft (76));
        streams.removeFromLeft (4);
        for (auto& button : streamButtons)
            button.setBounds (streams.removeFromLeft (30));
        streams.removeFromLeft (12);
        divideButton.setBounds (streams.removeFromLeft (80));
        streamStatus.setBounds (streams.reduced (4, 0));
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

    // Shows the controls the mode in use has a use for, and no others:
    // Delay belongs to Delay embed, the swing controls to Swing, the field
    // controls to Field (its curve only with the EQ on), and Colour and
    // Layout to the modes that draw curves.
    void updateVisibility()
    {
        const auto mode = (scope::Mode) juce::jlimit (0, 4, modeBox.getSelectedId() - 1);
        const bool swing = mode == scope::Mode::swing, field = mode == scope::Mode::field;
        const bool curve = field && fieldEqButton.getToggleState();
        const int state = (int) mode + (curve ? 16 : 0);
        if (state == shownState)
            return;
        shownState = state;
        showsSwing = swing;
        showsField = field;
        showsCurve = curve;

        auto show = [] (bool visible, std::initializer_list<juce::Component*> components)
        {
            for (auto* c : components)
                c->setVisible (visible);
        };
        show (mode == scope::Mode::delayEmbed, { &delay.label, &delay.slider });
        show (! field, { &colourBox, &layoutBox });
        show (swing, { &swingLabel, &swingMotionBox, &swingRate.label, &swingRate.slider, &swingSyncButton });
        show (field, { &fieldFade.label, &fieldFade.slider, &fieldSpeed.label, &fieldSpeed.slider, &fieldLockButton,
                       &fieldShapeBox, &fieldVolumeButton, &fieldDensity.label, &fieldDensity.slider,
                       &fieldFocus.label, &fieldFocus.slider, &fieldAperture.label, &fieldAperture.slider,
                       &fieldEqButton, &fieldEqWidth.label, &fieldEqWidth.slider });
        show (curve, { &fieldCurve });

        resized();
        if (onHeightChanged)
            onHeightChanged();
    }

    void timerCallback() override
    {
        updateVisibility();   // the host may have changed the mode

        // Of what is shown, some only does something with a switch on.
        swingRate.slider.setEnabled (! swingSyncButton.getToggleState());
        fieldSpeed.slider.setEnabled (fieldLockButton.getToggleState());
        fieldEqWidth.slider.setEnabled (fieldEqButton.getToggleState());
        fieldDensity.slider.setEnabled (fieldVolumeButton.getToggleState());
        fieldAperture.slider.setEnabled (fieldVolumeButton.getToggleState());
        fieldFocus.slider.setEnabled (fieldVolumeButton.getToggleState() && fieldAperture.slider.getValue() > 0.0);

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
    int shownState = -1;                             // the mode, and whether the EQ's curve shows
    bool showsSwing = false, showsField = false, showsCurve = false;

    juce::ComboBox modeBox, colourBox, layoutBox, blendBox, layerBox, swingMotionBox, planeBox, fieldShapeBox;
    juce::ToggleButton guidesButton { "Guides" }, testButton { "Test tone" }, divideButton { "Divide" },
                       swingSyncButton { "Once per window" }, fieldLockButton { "Lock to wave" }, fieldVolumeButton { "Volume" }, fieldEqButton { "EQ by distance" };
    FieldCurveEditor fieldCurve { processor.source };
    ColourScaleEditor colourScale;                   // the scene's, not this instance's
    juce::TextButton sumButton { "Sum with..." };
    juce::Label blendLabel, layerLabel, streamLabel, streamStatus, swingLabel, planeLabel, coloursLabel;
    std::array<juce::TextButton, net::maxStreams> streamButtons;

    LabelledSlider window, delay, afterglow, brightness, contrast, blur, lowCut, highCut, swingRate,
                   planeRotX, planeRotY, planeRotZ, viewYaw, viewPitch, viewRoll,
                   posX, posY, posZ, fieldFade, fieldSpeed, fieldDensity, fieldEqWidth, fieldFocus, fieldAperture, alpha;

    std::unique_ptr<ComboAttachment> modeAttachment, colourAttachment, layoutAttachment, blendAttachment, layerAttachment, swingMotionAttachment, planeAttachment, fieldShapeAttachment;
    std::unique_ptr<ButtonAttachment> guidesAttachment, testAttachment, divideAttachment, swingSyncAttachment, fieldLockAttachment, fieldVolumeAttachment, fieldEqAttachment;
    std::array<std::unique_ptr<ButtonAttachment>, net::maxStreams> streamAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};
