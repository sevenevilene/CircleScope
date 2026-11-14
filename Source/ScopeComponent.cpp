#include "ScopeComponent.h"

using namespace juce::gl;

namespace
{
    constexpr const char* lineVertexShader = R"(
        attribute vec2 position;
        attribute vec4 colour;
        uniform vec4 transform;   // xy scale, zw offset
        varying vec4 vColour;
        void main()
        {
            vColour = colour;
            gl_Position = vec4 (position * transform.xy + transform.zw, 0.0, 1.0);
        }
    )";

    constexpr const char* lineFragmentShader = R"(
        uniform float alphaScale;
        varying vec4 vColour;
        void main()
        {
            gl_FragColor = vec4 (vColour.rgb, vColour.a * alphaScale);
        }
    )";

    constexpr const char* fullScreenVertexShader = R"(
        attribute vec2 position;
        varying vec2 uv;
        void main()
        {
            uv = position * 0.5 + 0.5;
            gl_Position = vec4 (position, 0.0, 1.0);
        }
    )";

    // One direction of a separable Gaussian. Samples sourceA (+ sourceB when
    // mixB = 1) so the first pass can blur accum + current in one go.
    constexpr const char* blurFragmentShader = R"(
        uniform sampler2D sourceA;
        uniform sampler2D sourceB;
        uniform float mixB;
        uniform vec2 texelStep;
        uniform float sigma;
        uniform int radius;
        varying vec2 uv;
        void main()
        {
            vec3 sum = vec3 (0.0);
            float total = 0.0;
            for (int i = -radius; i <= radius; ++i)
            {
                float w = exp (-float (i * i) / (2.0 * sigma * sigma));
                vec2 p = uv + texelStep * float (i);
                sum += w * (texture2D (sourceA, p).rgb + mixB * texture2D (sourceB, p).rgb);
                total += w;
            }
            gl_FragColor = vec4 (sum / total, 1.0);
        }
    )";

    // Combine afterglow + live beam + blur halo, then tone map:
    // brightness scales light before a soft-saturating curve,
    // contrast is a power curve after it (higher = darker faint trails).
    constexpr const char* compositeFragmentShader = R"(
        uniform sampler2D accumulation;
        uniform sampler2D current;
        uniform sampler2D halo;
        uniform float exposure;
        uniform float contrast;
        uniform float haloGain;
        varying vec2 uv;
        void main()
        {
            vec3 c = texture2D (accumulation, uv).rgb
                   + texture2D (current, uv).rgb
                   + haloGain * texture2D (halo, uv).rgb;
            vec3 t = 1.0 - exp (-c * exposure);
            gl_FragColor = vec4 (pow (t, vec3 (contrast)), 1.0);
        }
    )";

    constexpr float plotRadius = 0.9f;     // where a full-scale pure tone lands
    constexpr int maxPointsPerChannel = 4096;
    constexpr int panelMargin = 16;        // logical px around each square
    constexpr float baseExposure = 1.2f;   // at 100 % brightness
    constexpr float haloGain = 1.5f;
    constexpr int maxBlurRadius = 48;
    constexpr int previewMaxWidth = 640;
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

    void bindTexture (int unit, GLuint texture)
    {
        glActiveTexture (GL_TEXTURE0 + (GLenum) unit);
        glBindTexture (GL_TEXTURE_2D, texture);
    }
}

//==============================================================================
void ScopeComponent::RenderTarget::ensure (int w, int h, bool halfFloat)
{
    w = juce::jmax (1, w);
    h = juce::jmax (1, h);
    if (fbo != 0 && w == width && h == height)
        return;

    release();

    // Half-float so slow decays fade smoothly to zero instead of leaving
    // 8-bit rounding ghosts, and bright overlaps can exceed 1.0.
    glGenTextures (1, &texture);
    glBindTexture (GL_TEXTURE_2D, texture);
    if (halfFloat)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    else
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture (GL_TEXTURE_2D, 0);

    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    jassert (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    width = w;
    height = h;
}

void ScopeComponent::RenderTarget::release()
{
    if (fbo != 0) glDeleteFramebuffers (1, &fbo);
    if (texture != 0) glDeleteTextures (1, &texture);
    fbo = texture = 0;
    width = height = 0;
}

void ScopeComponent::RenderTarget::bind() const
{
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glViewport (0, 0, width, height);
}

//==============================================================================
ScopeComponent::ScopeComponent (ScopeSource& s, bool labels) : source (&s), showLabels (labels)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        block[ch].resize ((size_t) blockSize);
        analytic[ch].resize ((size_t) blockSize);
    }
    timeScratch.resize ((size_t) blockSize);
    freqScratch.resize ((size_t) blockSize);

    setOpaque (true);

    // macOS only offers modern GL through a core profile. Elsewhere take the
    // driver's default context: a Raspberry Pi 4 has no 3.2 core profile, and
    // everything here also runs on GL 2.1 with the usual extensions.
   #if JUCE_MAC
    context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
   #endif
    context.setRenderer (this);
    context.setContinuousRepainting (true);
    context.setComponentPaintingEnabled (showLabels);
    context.attachTo (*this);

    if (showLabels)
        startTimerHz (10);
}

void ScopeComponent::setView (View v)
{
    viewX.store (v.x);
    viewY.store (v.y);
    viewW.store (juce::jmax (1.0e-4f, v.w));
    viewH.store (juce::jmax (1.0e-4f, v.h));
}

void ScopeComponent::setSource (ScopeSource& s)
{
    source.store (&s);
}

juce::String ScopeComponent::getRendererName() const
{
    const juce::SpinLock::ScopedLockType lock (rendererNameLock);
    return rendererName;
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
    const int layout = (int) source.load()->get (scope::pLayout);
    if (shownLayout.exchange (layout) != layout)
        repaint();
}

void ScopeComponent::resized()
{
    width.store (getWidth());
    height.store (getHeight());
}

void ScopeComponent::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick();
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
    {
        auto text = [] (GLenum name) { return juce::String::fromUTF8 ((const char*) glGetString (name)); };
        const auto name = text (GL_RENDERER) + ", GL " + text (GL_VERSION);
        juce::Logger::writeToLog ("CircleScope renderer: " + name);
        const juce::SpinLock::ScopedLockType lock (rendererNameLock);
        rendererName = name;
    }

    lineShader = buildShader (context, lineVertexShader, lineFragmentShader);
    blurShader = buildShader (context, fullScreenVertexShader, blurFragmentShader);
    compositeShader = buildShader (context, fullScreenVertexShader, compositeFragmentShader);
    if (lineShader == nullptr || blurShader == nullptr || compositeShader == nullptr)
    {
        lineShader.reset();
        blurShader.reset();
        compositeShader.reset();
        return;
    }

    linePosition = glGetAttribLocation (lineShader->getProgramID(), "position");
    lineColour = glGetAttribLocation (lineShader->getProgramID(), "colour");

    if (glGenVertexArrays != nullptr)   // optional on a GL 2.1 context
        glGenVertexArrays (1, &vao);
    glGenBuffers (1, &vbo);
}

void ScopeComponent::openGLContextClosing()
{
    for (auto* target : { &accumTarget, &currentTarget, &blurTempTarget, &blurTarget })
        target->release();
    for (auto& target : previewTargets)
        target.release();
    if (vbo != 0) glDeleteBuffers (1, &vbo);
    if (vao != 0) glDeleteVertexArrays (1, &vao);
    vbo = vao = 0;
    lineShader.reset();
    blurShader.reset();
    compositeShader.reset();
}

//==============================================================================
ScopeComponent::Settings ScopeComponent::readSettings() const
{
    const auto& src = *source.load();
    using namespace scope;

    return { src.get (pMode) > 0.5f,
             src.get (pColour) > 0.5f,
             src.get (pGuides) > 0.5f,
             (Layout) juce::jlimit (0, 2, (int) src.get (pLayout)),
             src.get (pWindow),
             src.get (pDelay),
             src.get (pAfterglow),
             src.get (pBrightness) * 0.01f,
             src.get (pContrast) * 0.01f,
             src.get (pBlur),
             src.get (pLowCut),
             src.get (pHighCut),
             juce::jmax (1000.0, src.sampleRate.load()) };
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

// Low cut / high cut as per-bin gains, with a half-octave raised-cosine
// slope at each edge so the band limits do not ring.
void ScopeComponent::updateBandWeights (const Settings& s)
{
    if (juce::exactlyEqual (s.lowCutHz, weightsLowCut) && juce::exactlyEqual (s.highCutHz, weightsHighCut)
        && juce::exactlyEqual (s.sampleRate, weightsSampleRate))
        return;

    weightsLowCut = s.lowCutHz;
    weightsHighCut = s.highCutHz;
    weightsSampleRate = s.sampleRate;

    const bool lowActive = s.lowCutHz > 20.5f, highActive = s.highCutHz < 19999.5f;
    bandLimited = lowActive || highActive;
    if (! bandLimited)
        return;

    // 0 well below the cut, 1 well above it.
    auto edge = [] (double hz, double cutHz)
    {
        const double t = juce::jlimit (0.0, 1.0, std::log2 (juce::jmax (hz, 1.0e-3) / cutHz) * 2.0 + 0.5);
        return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t);
    };

    bandWeights.resize ((size_t) blockSize / 2 + 1);
    for (int k = 0; k <= blockSize / 2; ++k)
    {
        const double hz = (double) k * s.sampleRate / (double) blockSize;
        double w = 1.0;
        if (lowActive)  w *= edge (hz, (double) s.lowCutHz);
        if (highActive) w *= 1.0 - edge (hz, (double) s.highCutHz);
        bandWeights[(size_t) k] = (float) w;
    }
}

void ScopeComponent::analyse (int channel, const Settings&)
{
    auto& samples = block[channel];
    source.load()->rings[channel].readLatest (samples.data(), blockSize);

    // Analytic signal: zero the negative frequencies, double the positive.
    for (int i = 0; i < blockSize; ++i)
        timeScratch[(size_t) i] = { samples[(size_t) i], 0.0f };
    fft.perform (timeScratch.data(), freqScratch.data(), false);

    if (bandLimited)
        for (int k = 0; k <= blockSize / 2; ++k)
            freqScratch[(size_t) k] *= bandWeights[(size_t) k];

    for (int k = 1; k < blockSize / 2; ++k)
        freqScratch[(size_t) k] *= 2.0f;
    for (int k = blockSize / 2 + 1; k < blockSize; ++k)
        freqScratch[(size_t) k] = 0.0f;
    fft.perform (freqScratch.data(), analytic[channel].data(), true);

    // Everything that reads the plain samples (delay embed, goniometer,
    // correlation) should see the band-limited signal too.
    if (bandLimited)
        for (int i = 0; i < blockSize; ++i)
            samples[(size_t) i] = analytic[channel][(size_t) i].real();
}

//==============================================================================
ScopeComponent::PanelPlacement ScopeComponent::placePanel (Layout layout, int panel, int w, int h, int margin) const
{
    // Lay the panels out on the whole virtual canvas, then shift by where
    // this component's part of it starts. GL's y axis points up.
    const float vx = viewX.load(), vy = viewY.load(), vw = viewW.load(), vh = viewH.load();
    const int canvasW = juce::roundToInt ((float) w / vw), canvasH = juce::roundToInt ((float) h / vh);
    const int originX = juce::roundToInt (vx * (float) canvasW);
    const int originY = juce::roundToInt ((1.0f - vy - vh) * (float) canvasH);

    const auto b = panelBounds (layout, panel, canvasW, canvasH, margin).translated (-originX, -originY);
    return { (float) b.getWidth() / (float) w,
             (float) b.getHeight() / (float) h,
             2.0f * (float) b.getCentreX() / (float) w - 1.0f,
             2.0f * (float) b.getCentreY() / (float) h - 1.0f,
             b.getIntersection ({ 0, 0, w, h }) };
}

// Aims the line shader at a panel. False if none of it is on this screen.
bool ScopeComponent::usePanel (const PanelPlacement& p)
{
    if (p.scissor.isEmpty())
        return false;

    glScissor (p.scissor.getX(), p.scissor.getY(), p.scissor.getWidth(), p.scissor.getHeight());
    setLineTransform (p.scaleX, p.scaleY, p.offsetX, p.offsetY);
    return true;
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
                                    std::optional<juce::Colour> colourOverride, float halfWidth)
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
        const Vertex v { x, y, col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(), 0.04f + 0.86f * age * age };
        if (havePrev)
            addSegment (prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

void ScopeComponent::addGoniometer (const Settings& s, const Range& r, float halfWidth)
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
                         0.04f + 0.86f * age * age };
        if (havePrev)
            addSegment (prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

//==============================================================================
void ScopeComponent::drawVertices (GLint positionAttrib, GLint colourAttrib)
{
    if (vertices.empty() || positionAttrib < 0)
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

void ScopeComponent::drawFullScreen (const juce::OpenGLShaderProgram& program)
{
    vertices.clear();
    addFullScreenQuad (0.0f, 0.0f, 0.0f, 0.0f);
    drawVertices (glGetAttribLocation (program.getProgramID(), "position"), -1);
}

void ScopeComponent::useLineShader (float alphaScale)
{
    lineShader->use();
    lineShader->setUniform ("alphaScale", alphaScale);
    setLineTransform (1.0f, 1.0f, 0.0f, 0.0f);
}

void ScopeComponent::setLineTransform (float scaleX, float scaleY, float offsetX, float offsetY)
{
    lineShader->setUniform ("transform", scaleX, scaleY, offsetX, offsetY);
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

    if (lineShader == nullptr || w <= 0 || h <= 0)
        return;

    const auto s = readSettings();
    const auto range = computeRange (s);

    // Frame timing drives the afterglow decay so it is frame-rate independent.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastFrameMs > 0.0 ? juce::jlimit (1.0, 100.0, now - lastFrameMs) : 16.0;
    lastFrameMs = now;
    const float keep = s.afterglowMs >= 1.0f ? (float) std::exp (-dt / (double) s.afterglowMs) : 0.0f;

    updateBandWeights (s);
    analyse (0, s);
    analyse (1, s);
    updateCorrelation (range);

    // Sizes that depend on the panel use the canvas, so every tile of a wall
    // draws lines of the same thickness.
    const int margin = juce::roundToInt ((float) panelMargin * scale);
    const int panels = numPanels (s.layout);
    const int firstSide = panelBounds (s.layout, 0, juce::roundToInt ((float) w / viewW.load()),
                                       juce::roundToInt ((float) h / viewH.load()), margin).getWidth();
    const float lineHalfWidth = 1.2f * scale / (float) firstSide;   // ~2.4 px thick, in NDC

    accumTarget.ensure (w, h);
    currentTarget.ensure (w, h);
    blurTempTarget.ensure (w / 2, h / 2);
    blurTarget.ensure (w / 2, h / 2);

    if (vao != 0)
        glBindVertexArray (vao);
    glEnable (GL_BLEND);

    // 1. Afterglow decays (dst *= keep); the live-beam buffer starts empty.
    accumTarget.bind();
    useLineShader (1.0f);
    glBlendFunc (GL_ZERO, GL_SRC_ALPHA);
    vertices.clear();
    addFullScreenQuad (0.0f, 0.0f, 0.0f, keep);
    drawVertices (linePosition, lineColour);

    currentTarget.bind();
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    // 2. This frame's traces: added to the afterglow at (1 - keep) so its
    //    steady state matches "off", and to the live beam at full strength
    //    so a shape that just moved is never dim.
    glBlendFunc (GL_SRC_ALPHA, GL_ONE);
    glEnable (GL_SCISSOR_TEST);
    for (int panel = 0; panel < panels; ++panel)
    {
        const auto placement = placePanel (s.layout, panel, w, h, margin);

        // Always build the vertices, even for a panel that is off this
        // screen: that keeps the auto-gain state running.
        vertices.clear();
        switch (s.layout)
        {
            case Layout::sideBySide:
                addTrajectory (panel, s, range, std::nullopt, lineHalfWidth);
                break;
            case Layout::overlay:
                addTrajectory (0, s, range, leftColour, lineHalfWidth);
                addTrajectory (1, s, range, rightColour, lineHalfWidth);
                break;
            case Layout::midSide:
                addGoniometer (s, range, lineHalfWidth);
                break;
        }

        for (auto* target : { &accumTarget, &currentTarget })
        {
            target->bind();
            useLineShader (target == &accumTarget ? 1.0f - keep : 1.0f);
            if (usePanel (placement))
                drawVertices (linePosition, lineColour);
        }
    }
    glDisable (GL_SCISSOR_TEST);

    // 3. Gaussian blur of (afterglow + beam) at half resolution, two passes.
    const int blurRadius = juce::jlimit (0, maxBlurRadius, juce::roundToInt (s.blurPx * scale * 0.5f));
    if (blurRadius > 0)
    {
        glDisable (GL_BLEND);
        blurShader->use();
        blurShader->setUniform ("sourceA", (GLint) 0);
        blurShader->setUniform ("sourceB", (GLint) 1);
        blurShader->setUniform ("sigma", juce::jmax (0.5f, (float) blurRadius / 2.5f));
        blurShader->setUniform ("radius", (GLint) blurRadius);

        blurTempTarget.bind();
        bindTexture (0, accumTarget.texture);
        bindTexture (1, currentTarget.texture);
        blurShader->setUniform ("mixB", 1.0f);
        blurShader->setUniform ("texelStep", 1.0f / (float) blurTempTarget.width, 0.0f);
        drawFullScreen (*blurShader);

        blurTarget.bind();
        bindTexture (0, blurTempTarget.texture);
        bindTexture (1, blurTempTarget.texture);
        blurShader->setUniform ("mixB", 0.0f);
        blurShader->setUniform ("texelStep", 0.0f, 1.0f / (float) blurTarget.height);
        drawFullScreen (*blurShader);
        glEnable (GL_BLEND);
    }

    // 4. Screen: guides (unaffected by image controls), then the composite on top.
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) screenFbo);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE);
    glViewport (0, 0, w, h);
    useLineShader (1.0f);
    if (s.guides)
    {
        glEnable (GL_SCISSOR_TEST);
        for (int panel = 0; panel < panels; ++panel)
        {
            if (! usePanel (placePanel (s.layout, panel, w, h, margin)))
                continue;

            vertices.clear();
            if (s.layout == Layout::midSide)
                addGoniometerGuides (lineHalfWidth * 0.5f);
            else
                addTrajectoryGuides (lineHalfWidth * 0.5f);
            drawVertices (linePosition, lineColour);
        }
        glDisable (GL_SCISSOR_TEST);
    }

    glViewport (0, 0, w, h);
    glBlendFunc (GL_ONE, GL_ONE);
    compositeShader->use();
    bindTexture (0, accumTarget.texture);
    bindTexture (1, currentTarget.texture);
    bindTexture (2, blurTarget.texture);
    compositeShader->setUniform ("accumulation", (GLint) 0);
    compositeShader->setUniform ("current", (GLint) 1);
    compositeShader->setUniform ("halo", (GLint) 2);
    compositeShader->setUniform ("exposure", baseExposure * s.brightness);
    compositeShader->setUniform ("contrast", s.contrast);
    compositeShader->setUniform ("haloGain", blurRadius > 0 ? haloGain : 0.0f);
    drawFullScreen (*compositeShader);

    for (int unit = 2; unit >= 0; --unit)
        bindTexture (unit, 0);

    capturePreview ((GLuint) screenFbo, w, h);

    if (vao != 0)
        glBindVertexArray (0);

    ++framesInWindow;
    if (now - fpsWindowStartMs >= 1000.0)
    {
        framesPerSecond.store (fpsWindowStartMs > 0.0 ? (float) (framesInWindow * 1000.0 / (now - fpsWindowStartMs)) : 0.0f);
        fpsWindowStartMs = now;
        framesInWindow = 0;
    }
}

// Shrinks the finished frame by repeated halving (each step a clean 2x2
// average) and hands the small copy to onPreview.
void ScopeComponent::capturePreview (GLuint screenFbo, int w, int h)
{
    const int interval = previewIntervalMs.load();
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (interval <= 0 || onPreview == nullptr || now - lastPreviewMs < (double) interval)
        return;
    lastPreviewMs = now;

    glDisable (GL_BLEND);
    GLuint from = screenFbo;
    int fromW = w, fromH = h;
    const RenderTarget* last = nullptr;

    for (auto& target : previewTargets)
    {
        const int toW = juce::jmax (1, fromW / 2), toH = juce::jmax (1, fromH / 2);
        target.ensure (toW, toH, false);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, from);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, target.fbo);
        glBlitFramebuffer (0, 0, fromW, fromH, 0, 0, toW, toH, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        last = &target;
        from = target.fbo;
        fromW = toW;
        fromH = toH;
        if (toW <= previewMaxWidth)
            break;
    }

    previewPixels.resize ((size_t) (fromW * fromH * 4));
    glBindFramebuffer (GL_READ_FRAMEBUFFER, last->fbo);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, fromW, fromH, GL_RGBA, GL_UNSIGNED_BYTE, previewPixels.data());
    glBindFramebuffer (GL_FRAMEBUFFER, screenFbo);
    glEnable (GL_BLEND);

    juce::Image image (juce::Image::RGB, fromW, fromH, false);
    {
        juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < fromH; ++y)
        {
            const auto* src = previewPixels.data() + (size_t) ((fromH - 1 - y) * fromW * 4);   // GL rows run bottom-up
            for (int x = 0; x < fromW; ++x, src += 4)
                pixels.setPixelColour (x, y, juce::Colour (src[0], src[1], src[2]));
        }
    }
    onPreview (image);
}
