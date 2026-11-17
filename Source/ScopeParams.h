#pragma once

// The one list of look/analysis parameters. The plugin builds its host
// parameters from it, the network stream carries the values in this order,
// receivers store them in this order, and MIDI CCs map through it.
//
// Blend, Layer and Divide only matter on a receiver (wall or cluster node),
// where every plugin instance sending to a stream is one layer of it:
//   Blend   how this layer is combined with the layers under it
//   Layer   stacking order, 1 at the bottom
//   Divide  on a wall, split one picture over the cells of all this
//           instance's streams instead of repeating it in each
//
// The Swing parameters belong to the Swing mode (see ScopeComponent.h).
//
// Plane and Plane rot X/Y/Z place the instance's drawing in the 3D scene;
// View yaw / pitch / roll are the camera, which turns the whole drawing. It
// is taken from the lowest layer that has turned it (see Plane.h). Camera
// X / Y / Z are the point in the scene it looks at and turns about, so
// moving that point moves the camera through the scene.
//
// Position X / Y / Z move the centre of the instance's plane in the scene.
//
// Field fade, Field lock and Field speed belong to the Field mode (see
// CurveAnalyser.h; its colours are the scene's, see ColourScale.h): how
// fast it fades with distance from the instance's position, whether the picture is locked to
// the wave, and how fast the locked picture moves on: a slow motion, in
// percent of real time. Field volume shows the field as a volume in space,
// looked through from the camera, instead of only where the planes cut it;
// Field density is how solid that volume is. Field shape is what the sound
// makes in space: dots that stand still, or rings that spread from the
// instance's position. Field EQ lets only a range of pitch into the field
// at each distance from the instance's position: which pitch is a curve
// the user draws (not a parameter: ScopeSource::fieldCurve), and Field EQ
// width is how wide the range is. Field aperture gives the volume a depth
// of field, like a camera lens: what is at the depth of Field focus is
// sharp, and the rest blurs the more the further it is from there.
//
// New parameters go at the end: a receiver takes as many values as it knows
// and leaves the rest at their defaults.
namespace scope
{
    enum ParamIndex
    {
        pMode = 0, pWindow, pDelay, pColour, pLayout, pAfterglow,
        pBrightness, pContrast, pBlur, pLowCut, pHighCut, pGuides, pTest,
        pBlend, pLayer, pDivide, pSwingMotion, pSwingRate, pSwingSync,
        pPlane, pPlaneRotX, pPlaneRotY, pPlaneRotZ, pViewYaw, pViewPitch, pViewRoll,
        pPosX, pPosY, pPosZ, pFieldFade, pFieldLock, pFieldSpeed, pFieldVolume, pFieldDensity, pFieldShape, pFieldEq, pFieldEqWidth, pFieldFocus, pFieldAperture,
        pCamX, pCamY, pCamZ,
        numParams
    };

    enum class ParamKind { choice, real, toggle };

    // The Field EQ. Its curve says which pitch belongs at which distance: it
    // has a value at fieldCurvePoints distances, evenly spaced from 0 to
    // fieldCurveSpan plane units (and the last one from there on), each 0..1
    // for a pitch from 20 Hz to about 20 kHz, in octaves. The sound is split
    // into fieldBands bands of fieldBandOctaves octaves to do it with.
    constexpr int fieldCurvePoints = 16;
    constexpr float fieldCurveSpan = 2.0f;
    constexpr int fieldBands = 8;
    constexpr float fieldBandOctaves = 1.25f;

    struct ParamInfo
    {
        const char* id;
        const char* name;
        ParamKind kind;
        float min, max, step, skew, def;
        const char* label;     // unit, for real parameters
        const char* choices;   // '|' separated, for choice parameters
        int cc;                // MIDI controller number (0..127 maps onto the full range), or -1 for none
    };

    enum class Layout { sideBySide = 0, overlay, midSide, mono };
    enum class Mode { quadrature = 0, delayEmbed, swing, axis, field };
    // What the samples of an instance are timed by. timeline: the host's
    // song position, while it plays. hostTime: the host's audio clock, when
    // it is stopped. Instances in one host agree on both.
    enum class Clock { none = 0, timeline, hostTime };
    enum class Plane { xy = 0, yz, xz };
    enum class Blend { add = 0, screen, normal, lighten, multiply, darken, overlay, difference, exclusion, subtract };

    // CCs 102..119, 85..90, 20..31, 14, 15, 3 and 9 are undefined in the MIDI spec, so they collide with nothing (25 is free again).
    inline constexpr ParamInfo paramTable[numParams] = {
        { "mode",       "Mode",       ParamKind::choice, 0.0f,  4.0f,     1.0f,  1.0f,  0.0f,     "",   "Quadrature|Delay embed|Swing|Axis|Field", 102 },
        { "window",     "Window",     ParamKind::real,   5.0f,  300.0f,   0.1f,  0.5f,  60.0f,    "ms", "",                                103 },
        { "delay",      "Delay",      ParamKind::real,   0.05f, 50.0f,    0.01f, 0.4f,  12.5f,    "ms", "",                                104 },
        { "colour",     "Colour",     ParamKind::choice, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "Frequency hue|Fixed",             105 },
        { "layout",     "Layout",     ParamKind::choice, 0.0f,  3.0f,     1.0f,  1.0f,  0.0f,     "",   "Side by side|Overlay|Mid / side|Mono", 106 },
        { "afterglow",  "Afterglow",  ParamKind::real,   0.0f,  2000.0f,  1.0f,  0.4f,  250.0f,   "ms", "",                                107 },
        { "brightness", "Brightness", ParamKind::real,   10.0f, 800.0f,   1.0f,  0.5f,  100.0f,   "%",  "",                                108 },
        { "contrast",   "Contrast",   ParamKind::real,   25.0f, 300.0f,   1.0f,  0.6f,  100.0f,   "%",  "",                                109 },
        { "blur",       "Blur",       ParamKind::real,   0.0f,  30.0f,    0.1f,  0.6f,  4.0f,     "px", "",                                110 },
        { "lowcut",     "Low cut",    ParamKind::real,   20.0f, 20000.0f, 1.0f,  0.25f, 20.0f,    "Hz", "",                                111 },
        { "highcut",    "High cut",   ParamKind::real,   20.0f, 20000.0f, 1.0f,  0.25f, 20000.0f, "Hz", "",                                112 },
        { "guides",     "Guides",     ParamKind::toggle, 0.0f,  1.0f,     1.0f,  1.0f,  1.0f,     "",   "",                                113 },
        { "test",       "Test tone",  ParamKind::toggle, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "",                                114 },
        { "blend",      "Blend",      ParamKind::choice, 0.0f,  9.0f,     1.0f,  1.0f,  0.0f,     "",   "Add|Screen|Normal|Lighten|Multiply|Darken|Overlay|Difference|Exclusion|Subtract", 115 },
        { "layer",      "Layer",      ParamKind::choice, 0.0f,  7.0f,     1.0f,  1.0f,  0.0f,     "",   "1|2|3|4|5|6|7|8",                 116 },
        { "divide",     "Divide",     ParamKind::toggle, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "",                                117 },
        { "swingmotion","Swing motion", ParamKind::choice, 0.0f, 1.0f,    1.0f,  1.0f,  0.0f,     "",   "Back and forth|Round",            118 },
        { "swingrate",  "Swing rate", ParamKind::real,   0.05f, 2000.0f,  0.01f, 0.3f,  1.0f,     "Hz", "",                                119 },
        { "swingsync",  "Swing per window", ParamKind::toggle, 0.0f, 1.0f, 1.0f, 1.0f,  1.0f,     "",   "",                                85 },
        { "plane",      "Plane",      ParamKind::choice, 0.0f,  2.0f,     1.0f,  1.0f,  0.0f,     "",   "XY|YZ|XZ",                        86 },
        { "planerotx",  "Plane rot X", ParamKind::real, -180.0f, 180.0f,  0.1f,  1.0f,  0.0f,     "deg", "",                               87 },
        { "planeroty",  "Plane rot Y", ParamKind::real, -180.0f, 180.0f,  0.1f,  1.0f,  0.0f,     "deg", "",                               88 },
        { "planerotz",  "Plane rot Z", ParamKind::real, -180.0f, 180.0f,  0.1f,  1.0f,  0.0f,     "deg", "",                               89 },
        { "viewyaw",    "View yaw",   ParamKind::real, -180.0f, 180.0f,   0.1f,  1.0f,  0.0f,     "deg", "",                               90 },
        { "viewpitch",  "View pitch", ParamKind::real,  -90.0f,  90.0f,   0.1f,  1.0f,  0.0f,     "deg", "",                               20 },
        { "viewroll",   "View roll",  ParamKind::real, -180.0f, 180.0f,   0.1f,  1.0f,  0.0f,     "deg", "",                               21 },
        { "posx",       "Position X", ParamKind::real,   -2.0f,   2.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                22 },
        { "posy",       "Position Y", ParamKind::real,   -2.0f,   2.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                23 },
        { "posz",       "Position Z", ParamKind::real,   -2.0f,   2.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                24 },
        { "fieldfade",  "Field fade", ParamKind::real,    0.0f, 100.0f,   0.1f,  1.0f,  45.0f,    "%",  "",                                26 },
        { "fieldlock",  "Field lock", ParamKind::toggle,  0.0f,   1.0f,   1.0f,  1.0f,  1.0f,     "",   "",                                27 },
        { "fieldspeed", "Field speed", ParamKind::real,   0.0f, 100.0f,   0.01f, 0.3f,  1.0f,     "%",  "",                                28 },
        { "fieldvolume", "Field volume", ParamKind::toggle, 0.0f, 1.0f,   1.0f,  1.0f,  1.0f,     "",   "",                                29 },
        { "fielddensity", "Field density", ParamKind::real, 0.0f, 100.0f, 0.1f,  1.0f,  50.0f,    "%",  "",                                30 },
        { "fieldshape", "Field shape", ParamKind::choice, 0.0f,  1.0f,    1.0f,  1.0f,  0.0f,     "",   "Dots|Rings",                      31 },
        { "fieldeq",    "Field EQ",   ParamKind::toggle,  0.0f,   1.0f,   1.0f,  1.0f,  0.0f,     "",   "",                                14 },
        { "fieldeqwidth", "Field EQ width", ParamKind::real, 0.3f, 10.0f, 0.01f, 0.5f,  2.0f,     "oct", "",                               15 },
        { "fieldfocus", "Field focus", ParamKind::real,  -3.0f,   3.0f,   0.01f, 1.0f,  0.0f,     "",   "",                                3 },
        { "fieldaperture", "Field aperture", ParamKind::real, 0.0f, 100.0f, 0.1f, 1.0f, 0.0f,     "%",  "",                                9 },
        { "camx",       "Camera X",   ParamKind::real,  -20.0f,  20.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                -1 },
        { "camy",       "Camera Y",   ParamKind::real,  -20.0f,  20.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                -1 },
        { "camz",       "Camera Z",   ParamKind::real,  -20.0f,  20.0f,   0.001f, 1.0f, 0.0f,     "",   "",                                -1 },
    };
}
