#pragma once

// The one list of look/analysis parameters. The plugin builds its host
// parameters from it, the network stream carries the values in this order,
// cluster nodes store them in this order, and MIDI CCs map through it.
namespace scope
{
    enum ParamIndex
    {
        pMode = 0, pWindow, pDelay, pColour, pLayout, pAfterglow,
        pBrightness, pContrast, pBlur, pLowCut, pHighCut, pGuides, pTest,
        numParams
    };

    enum class ParamKind { choice, real, toggle };

    struct ParamInfo
    {
        const char* id;
        const char* name;
        ParamKind kind;
        float min, max, step, skew, def;
        const char* label;     // unit, for real parameters
        const char* choices;   // '|' separated, for choice parameters
        int cc;                // MIDI controller number (0..127 maps onto the full range)
    };

    // CCs 102..114 are undefined in the MIDI spec, so they collide with nothing.
    inline constexpr ParamInfo paramTable[numParams] = {
        { "mode",       "Mode",       ParamKind::choice, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "Quadrature|Delay embed",          102 },
        { "window",     "Window",     ParamKind::real,   5.0f,  300.0f,   0.1f,  0.5f,  60.0f,    "ms", "",                                103 },
        { "delay",      "Delay",      ParamKind::real,   0.05f, 50.0f,    0.01f, 0.4f,  12.5f,    "ms", "",                                104 },
        { "colour",     "Colour",     ParamKind::choice, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "Frequency hue|Fixed",             105 },
        { "layout",     "Layout",     ParamKind::choice, 0.0f,  2.0f,     1.0f,  1.0f,  0.0f,     "",   "Side by side|Overlay|Mid / side", 106 },
        { "afterglow",  "Afterglow",  ParamKind::real,   0.0f,  2000.0f,  1.0f,  0.4f,  250.0f,   "ms", "",                                107 },
        { "brightness", "Brightness", ParamKind::real,   10.0f, 800.0f,   1.0f,  0.5f,  100.0f,   "%",  "",                                108 },
        { "contrast",   "Contrast",   ParamKind::real,   25.0f, 300.0f,   1.0f,  0.6f,  100.0f,   "%",  "",                                109 },
        { "blur",       "Blur",       ParamKind::real,   0.0f,  30.0f,    0.1f,  0.6f,  4.0f,     "px", "",                                110 },
        { "lowcut",     "Low cut",    ParamKind::real,   20.0f, 20000.0f, 1.0f,  0.25f, 20.0f,    "Hz", "",                                111 },
        { "highcut",    "High cut",   ParamKind::real,   20.0f, 20000.0f, 1.0f,  0.25f, 20000.0f, "Hz", "",                                112 },
        { "guides",     "Guides",     ParamKind::toggle, 0.0f,  1.0f,     1.0f,  1.0f,  1.0f,     "",   "",                                113 },
        { "test",       "Test tone",  ParamKind::toggle, 0.0f,  1.0f,     1.0f,  1.0f,  0.0f,     "",   "",                                114 },
    };
}
