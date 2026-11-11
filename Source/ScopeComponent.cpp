#include "ScopeComponent.h"

using namespace juce::gl;

namespace
{
    constexpr const char* lineVertexShader = R"(
        attribute vec2 position;
        attribute vec4 colour;
        varying vec4 vColour;
        void main()
        {
            vColour = colour;
            gl_Position = vec4 (position, 0.0, 1.0);
        }
    )";

    constexpr const char* lineFragmentShader = R"(
        varying vec4 vColour;
        void main()
        {
            gl_FragColor = vColour;
        }
    )";

    constexpr const char* glowVertexShader = R"(
        attribute vec2 position;
        varying vec2 uv;
        void main()
        {
            uv = position * 0.5 + 0.5;
            gl_Position = vec4 (position, 0.0, 1.0);
        }
    )";

    // Soft-saturating tone map: overlapping passes glow instead of clipping.
    constexpr const char* glowFragmentShader = R"(
        uniform sampler2D accumulation;
        uniform float exposure;
        varying vec2 uv;
        void main()
        {
            vec3 c = texture2D (accumulation, uv).rgb;
            gl_FragColor = vec4 (1.0 - exp (-c * exposure), 1.0);
        }
    )";

    constexpr float plotRadius = 0.9f;     // where a full-scale pure tone lands
    constexpr int maxPointsPerChannel = 4096;
    constexpr int panelMargin = 16;        // logical px around each square
    constexpr float exposure = 2.0f;
    constexpr float barY = -0.95f;         // correlation bar height in the goniometer

    const juce::Colour leftColour (0xff1d9e75), rightColour (0xffd85a30), goniometerColour (0xff7f77dd);
    const juce::Colour backgroundColour (0xff09090b);

    std::unique_ptr<juce::OpenGLShaderProgram> buildShader (juce::OpenGLContext& ctx, const char* vs, const char* fs)
    {
        auto program = std::make_unique<juce::OpenGLShaderProgram> (ctx);
        if (program->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vs))
            && program->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fs))
            && program->link())
            return program;

        DBG ("Shader error: " << program->getLastError());
        return nullptr;
    }
}

ScopeComponent::ScopeComponent (CircleScopeProcessor& p) : processor (p)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        block[ch].resize ((size_t) blockSize);
        analytic[ch].resize ((size_t) blockSize);
    }
    timeScratch.resize ((size_t) blockSize);
    freqScratch.resize ((size_t) blockSize);

    setOpaque (true);
    context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    context.setRenderer (this);
    context.setContinuousRepainting (true);
    context.setComponentPaintingEnabled (true);   // for the text labels
    context.attachTo (*this);

    startTimerHz (10);
}

ScopeComponent::~ScopeComponent()
{
    stopTimer();
    context.detach();
}

//==============================================================================
int ScopeComponent::numPanels (Layout layout)
{
    return layout == Layout::sideBySide ? 2 : 1;
}

juce::Rectangle<int> ScopeComponent::panelBounds (Layout layout, int panel, int w, int h, int margin)
{
    if (layout == Layout::sideBySide)
    {
        const int halfW = w / 2;
        const int side = juce::jmax (1, juce::jmin (halfW, h) - margin);
        return { panel * halfW + (halfW - side) / 2, (h - side) / 2, side, side };
    }

    const int side = juce::jmax (1, juce::jmin (w, h) - margin);
    return { (w - side) / 2, (h - side) / 2, side, side };
}

juce::Colour ScopeComponent::colourForFrequency (double hz)
{
    // 20 Hz = red ... 20 kHz = blue, log spaced.
    const double u = juce::jlimit (0.0, 1.0, std::log (juce::jmax (hz, 1.0) / 20.0) / std::log (1000.0));
    return juce::Colour::fromHSV (0.66f * (float) u, 0.8f, 1.0f, 1.0f);
}

//==============================================================================
void ScopeComponent::timerCallback()
{
    const int layout = (int) processor.apvts.getRawParameterValue ("layout")->load();
    if (shownLayout.exchange (layout) != layout)
        repaint();
}

void ScopeComponent::resized()
{
    width.store (getWidth());
    height.store (getHeight());
}

void ScopeComponent::paint (juce::Graphics& g)
{
    const auto layout = (Layout) juce::jlimit (0, 2, shownLayout.load());
    g.setFont (14.0f);

    if (layout == Layout::sideBySide)
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        const auto half = getLocalBounds().withWidth (getWidth() / 2).reduced (12);
        g.drawText ("L", half, juce::Justification::topLeft);
        g.drawText ("R", half.translated (getWidth() / 2, 0), juce::Justification::topLeft);
        return;
    }

    if (layout == Layout::overlay)
    {
        const auto area = getLocalBounds().reduced (12);
        g.setColour (leftColour);
        g.drawText ("L", area, juce::Justification::topLeft);
        g.setColour (rightColour);
        g.drawText ("R", area.translated (16, 0), juce::Justification::topLeft);
        return;
    }

    // Mid / side: label the axes inside the square.
    const auto panel = panelBounds (layout, 0, getWidth(), getHeight(), panelMargin).toFloat();
    auto toPx = [&panel] (float nx, float ny)
    {
        return juce::Point<float> (panel.getX() + (nx * 0.5f + 0.5f) * panel.getWidth(),
                                   panel.getY() + (0.5f - ny * 0.5f) * panel.getHeight());
    };
    auto label = [&g, &toPx] (const juce::String& text, float nx, float ny)
    {
        g.drawText (text, juce::Rectangle<float> (40.0f, 18.0f).withCentre (toPx (nx, ny)),
                    juce::Justification::centred);
    };

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (13.0f);
    label ("M", 0.0f, 0.97f);
    label ("S", 0.96f, 0.05f);
    label ("L", -0.68f, 0.72f);
    label ("R", 0.68f, 0.72f);
    g.setFont (11.0f);
    label ("-1", -0.85f, barY + 0.07f);
    label ("0", 0.0f, barY + 0.07f);
    label ("+1", 0.85f, barY + 0.07f);
}

//==============================================================================
void ScopeComponent::newOpenGLContextCreated()
{
    lineShader = buildShader (context, lineVertexShader, lineFragmentShader);
    glowShader = buildShader (context, glowVertexShader, glowFragmentShader);
    if (lineShader == nullptr || glowShader == nullptr)
    {
        lineShader.reset();
        glowShader.reset();
        return;
    }

    linePosition = glGetAttribLocation (lineShader->getProgramID(), "position");
    lineColour = glGetAttribLocation (lineShader->getProgramID(), "colour");
    glowPosition = glGetAttribLocation (glowShader->getProgramID(), "position");
    glowTexture = glGetUniformLocation (glowShader->getProgramID(), "accumulation");
    glowExposure = glGetUniformLocation (glowShader->getProgramID(), "exposure");

    glGenVertexArrays (1, &vao);
    glGenBuffers (1, &vbo);
}

void ScopeComponent::openGLContextClosing()
{
    releaseFramebuffer();
    if (vbo != 0) glDeleteBuffers (1, &vbo);
    if (vao != 0) glDeleteVertexArrays (1, &vao);
    vbo = vao = 0;
    lineShader.reset();
    glowShader.reset();
}

void ScopeComponent::ensureFramebuffer (int w, int h)
{
    if (fbo != 0 && w == fboWidth && h == fboHeight)
        return;

    releaseFramebuffer();

    // Half-float so slow decays fade smoothly to zero instead of leaving
    // 8-bit rounding ghosts.
    glGenTextures (1, &fboTexture);
    glBindTexture (GL_TEXTURE_2D, fboTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture (GL_TEXTURE_2D, 0);

    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTexture, 0);
    jassert (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    fboWidth = w;
    fboHeight = h;
}

void ScopeComponent::releaseFramebuffer()
{
    if (fbo != 0) glDeleteFramebuffers (1, &fbo);
    if (fboTexture != 0) glDeleteTextures (1, &fboTexture);
    fbo = fboTexture = 0;
    fboWidth = fboHeight = 0;
}

//==============================================================================
ScopeComponent::Settings ScopeComponent::readSettings() const
{
    auto& apvts = processor.apvts;
    auto value = [&apvts] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    return { value ("mode") > 0.5f,
             value ("colour") > 0.5f,
             (Layout) juce::jlimit (0, 2, (int) value ("layout")),
             value ("window"),
             value ("delay"),
             value ("afterglow"),
             processor.currentSampleRate.load() };
}

ScopeComponent::Range ScopeComponent::computeRange (const Settings& s) const
{
    Range r {};
    r.end = blockSize - edgeMargin;
    r.length = juce::jlimit (16, blockSize - 2 * edgeMargin, (int) (s.windowMs * 0.001 * s.sampleRate));
    r.start = r.end - r.length;
    r.delay = juce::jlimit (1, r.start, (int) (s.delayMs * 0.001 * s.sampleRate));
    r.stride = juce::jmax (1, r.length / maxPointsPerChannel);
    r.freqLag = juce::jmax (r.stride, 8);
    return r;
}

void ScopeComponent::analyse (int channel)
{
    auto& samples = block[channel];
    processor.rings[channel].readLatest (samples.data(), blockSize);

    // Analytic signal: zero the negative frequencies, double the positive.
    for (int i = 0; i < blockSize; ++i)
        timeScratch[(size_t) i] = { samples[(size_t) i], 0.0f };
    fft.perform (timeScratch.data(), freqScratch.data(), false);
    for (int k = 1; k < blockSize / 2; ++k)
        freqScratch[(size_t) k] *= 2.0f;
    for (int k = blockSize / 2 + 1; k < blockSize; ++k)
        freqScratch[(size_t) k] = 0.0f;
    fft.perform (freqScratch.data(), analytic[channel].data(), true);
}

void ScopeComponent::updateCorrelation (const Range& r)
{
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = r.start; i < r.end; ++i)
    {
        const double l = block[0][(size_t) i], rt = block[1][(size_t) i];
        lr += l * rt;
        ll += l * l;
        rr += rt * rt;
    }

    const double energy = std::sqrt (ll * rr);
    const float target = energy > 1.0e-12 ? (float) (lr / energy) : 1.0f;
    correlation += (target - correlation) * 0.2f;
}

//==============================================================================
void ScopeComponent::addSegment (Vertex a, Vertex b, float halfWidth)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt (dx * dx + dy * dy);
    if (len < 1.0e-6f)
        return;

    const float nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
    auto offset = [] (Vertex v, float ox, float oy) { v.x += ox; v.y += oy; return v; };

    const Vertex a0 = offset (a, nx, ny), a1 = offset (a, -nx, -ny);
    const Vertex b0 = offset (b, nx, ny), b1 = offset (b, -nx, -ny);
    vertices.insert (vertices.end(), { a0, a1, b0, b0, a1, b1 });
}

void ScopeComponent::addFullScreenQuad (float r, float g, float b, float a)
{
    auto v = [=] (float x, float y) { return Vertex { x, y, r, g, b, a }; };
    vertices.insert (vertices.end(), { v (-1, -1), v (1, -1), v (1, 1), v (-1, -1), v (1, 1), v (-1, 1) });
}

void ScopeComponent::addTrajectoryGuides (float halfWidth)
{
    const float c = 0.18f;
    auto v = [c] (float x, float y) { return Vertex { x, y, c, c, c * 1.1f, 1.0f }; };

    addSegment (v (-1.0f, 0.0f), v (1.0f, 0.0f), halfWidth);
    addSegment (v (0.0f, -1.0f), v (0.0f, 1.0f), halfWidth);

    constexpr int steps = 128;
    for (int i = 0; i < steps; ++i)
    {
        const float a0 = juce::MathConstants<float>::twoPi * (float) i / steps;
        const float a1 = juce::MathConstants<float>::twoPi * (float) (i + 1) / steps;
        addSegment (v (plotRadius * std::cos (a0), plotRadius * std::sin (a0)),
                    v (plotRadius * std::cos (a1), plotRadius * std::sin (a1)), halfWidth);
    }
}

void ScopeComponent::addGoniometerGuides (float halfWidth)
{
    const float c = 0.18f;
    auto v = [c] (float x, float y, float k = 1.0f) { return Vertex { x, y, c * k, c * k, c * 1.1f * k, 1.0f }; };

    const float d = plotRadius * 0.7071f;
    addSegment (v (0.0f, -plotRadius), v (0.0f, plotRadius), halfWidth);           // mono (M)
    addSegment (v (-plotRadius, 0.0f), v (plotRadius, 0.0f), halfWidth);           // anti-phase (S)
    addSegment (v (-d, -d, 0.6f), v (d, d, 0.6f), halfWidth);                       // R only
    addSegment (v (-d, d, 0.6f), v (d, -d, 0.6f), halfWidth);                       // L only

    // Correlation bar: track, centre tick, and a marker coloured by sign.
    const float barHalf = 0.85f;
    addSegment (v (-barHalf, barY), v (barHalf, barY), halfWidth);
    addSegment (v (0.0f, barY - 0.02f), v (0.0f, barY + 0.02f), halfWidth);

    const auto markerColour = correlation > 0.3f ? juce::Colour (0xff97c459)
                            : correlation >= 0.0f ? juce::Colour (0xffef9f27)
                                                  : juce::Colour (0xffe24b4a);
    const float mx = barHalf * juce::jlimit (-1.0f, 1.0f, correlation);
    const Vertex top { mx, barY + 0.035f, markerColour.getFloatRed(), markerColour.getFloatGreen(), markerColour.getFloatBlue(), 1.0f };
    Vertex bottom = top;
    bottom.y = barY - 0.035f;
    addSegment (bottom, top, halfWidth * 3.0f);
}

void ScopeComponent::addTrajectory (int channel, const Settings& s, const Range& r,
                                    std::optional<juce::Colour> colourOverride, float alphaScale, float halfWidth)
{
    const auto& z = analytic[channel];
    const auto& samples = block[channel];

    // Auto-gain: snap down on loud input, ease back up when it gets quieter.
    float peak = 0.0f;
    for (int i = r.start; i < r.end; ++i)
        peak = juce::jmax (peak, std::abs (z[(size_t) i]));
    const float target = plotRadius / juce::jmax (peak, 0.01f);
    gain[channel] = target < gain[channel] ? target : gain[channel] + (target - gain[channel]) * 0.05f;
    const float g = gain[channel];

    const juce::Colour fixed = colourOverride.value_or (channel == 0 ? leftColour : rightColour);
    const bool useHue = ! s.fixedColour && ! colourOverride.has_value();

    Vertex prev {};
    bool havePrev = false;
    for (int i = r.start; i < r.end; i += r.stride)
    {
        const auto zi = z[(size_t) i];
        const float x = zi.real() * g;
        const float y = s.delayMode ? samples[(size_t) (i - r.delay)] * g : zi.imag() * g;

        juce::Colour col = fixed;
        if (useHue)
        {
            // Instantaneous frequency from the phase advance over freqLag samples.
            const auto rot = zi * std::conj (z[(size_t) (i - r.freqLag)]);
            col = colourForFrequency (std::abs (std::arg (rot)) * s.sampleRate
                                      / (juce::MathConstants<double>::twoPi * r.freqLag));
        }

        const float age = (float) (i - r.start) / (float) r.length;
        const Vertex v { x, y, col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(),
                         (0.04f + 0.86f * age * age) * alphaScale };
        if (havePrev)
            addSegment (prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

void ScopeComponent::addGoniometer (const Settings& s, const Range& r, float alphaScale, float halfWidth)
{
    const auto& left = block[0];
    const auto& right = block[1];
    constexpr float k = 0.70710678f;

    float peak = 0.0f;
    for (int i = r.start; i < r.end; ++i)
    {
        const float l = left[(size_t) i], rt = right[(size_t) i];
        peak = juce::jmax (peak, std::abs (l + rt) * k, std::abs (rt - l) * k);
    }
    const float target = plotRadius / juce::jmax (peak, 0.01f);
    gain[2] = target < gain[2] ? target : gain[2] + (target - gain[2]) * 0.05f;
    const float g = gain[2] * k;

    Vertex prev {};
    bool havePrev = false;
    for (int i = r.start; i < r.end; i += r.stride)
    {
        const float l = left[(size_t) i], rt = right[(size_t) i];

        juce::Colour col = goniometerColour;
        if (! s.fixedColour)
        {
            // Hue from the mid signal's instantaneous frequency.
            const auto zm = analytic[0][(size_t) i] + analytic[1][(size_t) i];
            const auto zmPrev = analytic[0][(size_t) (i - r.freqLag)] + analytic[1][(size_t) (i - r.freqLag)];
            col = colourForFrequency (std::abs (std::arg (zm * std::conj (zmPrev))) * s.sampleRate
                                      / (juce::MathConstants<double>::twoPi * r.freqLag));
        }

        const float age = (float) (i - r.start) / (float) r.length;
        const Vertex v { (rt - l) * g, (l + rt) * g, col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(),
                         (0.04f + 0.86f * age * age) * alphaScale };
        if (havePrev)
            addSegment (prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

void ScopeComponent::drawVertices (GLint positionAttrib, GLint colourAttrib)
{
    if (vertices.empty())
        return;

    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (vertices.size() * sizeof (Vertex)), vertices.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray ((GLuint) positionAttrib);
    glVertexAttribPointer ((GLuint) positionAttrib, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex), nullptr);
    if (colourAttrib >= 0)
    {
        glEnableVertexAttribArray ((GLuint) colourAttrib);
        glVertexAttribPointer ((GLuint) colourAttrib, 4, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                               reinterpret_cast<void*> (2 * sizeof (float)));
    }

    glDrawArrays (GL_TRIANGLES, 0, (GLsizei) vertices.size());

    glDisableVertexAttribArray ((GLuint) positionAttrib);
    if (colourAttrib >= 0)
        glDisableVertexAttribArray ((GLuint) colourAttrib);
}

//==============================================================================
void ScopeComponent::renderOpenGL()
{
    const float scale = (float) context.getRenderingScale();
    const int w = juce::roundToInt ((float) width.load() * scale);
    const int h = juce::roundToInt ((float) height.load() * scale);

    GLint screenFbo = 0;
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &screenFbo);

    glViewport (0, 0, w, h);
    glClearColor (backgroundColour.getFloatRed(), backgroundColour.getFloatGreen(), backgroundColour.getFloatBlue(), 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (lineShader == nullptr || glowShader == nullptr || w <= 0 || h <= 0)
        return;

    const auto s = readSettings();
    const auto range = computeRange (s);

    // Frame timing drives the afterglow decay so it is frame-rate independent.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastFrameMs > 0.0 ? juce::jlimit (1.0, 100.0, now - lastFrameMs) : 16.0;
    lastFrameMs = now;
    const float keep = s.afterglowMs >= 1.0f ? (float) std::exp (-dt / (double) s.afterglowMs) : 0.0f;
    const float alphaScale = 1.0f - keep;   // steady-state brightness matches "off"

    analyse (0);
    analyse (1);
    updateCorrelation (range);

    const int margin = juce::roundToInt ((float) panelMargin * scale);
    const int panels = numPanels (s.layout);
    const int firstSide = panelBounds (s.layout, 0, w, h, margin).getWidth();
    const float lineHalfWidth = 1.2f * scale / (float) firstSide;   // ~2.4 px thick, in NDC

    glBindVertexArray (vao);
    glEnable (GL_BLEND);

    // 1. Decay the accumulation buffer, then add this frame's traces to it.
    ensureFramebuffer (w, h);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glViewport (0, 0, w, h);
    lineShader->use();

    glBlendFunc (GL_ZERO, GL_SRC_ALPHA);   // dst *= keep
    vertices.clear();
    addFullScreenQuad (0.0f, 0.0f, 0.0f, keep);
    drawVertices (linePosition, lineColour);

    glBlendFunc (GL_SRC_ALPHA, GL_ONE);    // additive
    for (int panel = 0; panel < panels; ++panel)
    {
        const auto b = panelBounds (s.layout, panel, w, h, margin);
        glViewport (b.getX(), b.getY(), b.getWidth(), b.getHeight());
        vertices.clear();

        switch (s.layout)
        {
            case Layout::sideBySide:
                addTrajectory (panel, s, range, std::nullopt, alphaScale, lineHalfWidth);
                break;
            case Layout::overlay:
                addTrajectory (0, s, range, leftColour, alphaScale, lineHalfWidth);
                addTrajectory (1, s, range, rightColour, alphaScale, lineHalfWidth);
                break;
            case Layout::midSide:
                addGoniometer (s, range, alphaScale, lineHalfWidth);
                break;
        }
        drawVertices (linePosition, lineColour);
    }

    // 2. Screen: guides first (not accumulated), then the tone-mapped glow on top.
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) screenFbo);
    for (int panel = 0; panel < panels; ++panel)
    {
        const auto b = panelBounds (s.layout, panel, w, h, margin);
        glViewport (b.getX(), b.getY(), b.getWidth(), b.getHeight());
        vertices.clear();
        if (s.layout == Layout::midSide)
            addGoniometerGuides (lineHalfWidth * 0.5f);
        else
            addTrajectoryGuides (lineHalfWidth * 0.5f);
        drawVertices (linePosition, lineColour);
    }

    glViewport (0, 0, w, h);
    glowShader->use();
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, fboTexture);
    glUniform1i (glowTexture, 0);
    glUniform1f (glowExposure, exposure);
    glBlendFunc (GL_ONE, GL_ONE);
    vertices.clear();
    addFullScreenQuad (0.0f, 0.0f, 0.0f, 0.0f);
    drawVertices (glowPosition, -1);
    glBindTexture (GL_TEXTURE_2D, 0);

    glBindVertexArray (0);
}
