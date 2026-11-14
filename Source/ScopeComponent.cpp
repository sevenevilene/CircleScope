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

    // One layer onto what is under it. The layer's picture is its afterglow +
    // live beam + blur halo, tone mapped: brightness scales light before a
    // soft-saturating curve, contrast is a power curve after it (higher =
    // darker faint trails). Then it is combined with the canvas below by its
    // blend mode (scope::Blend). A layer is light on black, so the modes in
    // which black would wipe out what is below only act where the layer has
    // light: its brightness serves as its opacity.
    constexpr const char* blendFragmentShader = R"(
        uniform sampler2D below;
        uniform sampler2D accumulation;
        uniform sampler2D current;
        uniform sampler2D halo;
        uniform float exposure;
        uniform float contrast;
        uniform float haloGain;
        uniform int mode;
        varying vec2 uv;
        void main()
        {
            vec3 c = texture2D (accumulation, uv).rgb
                   + texture2D (current, uv).rgb
                   + haloGain * texture2D (halo, uv).rgb;
            vec3 s = pow (1.0 - exp (-c * exposure), vec3 (contrast));
            vec3 d = clamp (texture2D (below, uv).rgb, 0.0, 1.0);
            float a = clamp (max (s.r, max (s.g, s.b)), 0.0, 1.0);

            vec3 o = d + s;                                                   // add
            if (mode == 1)      o = 1.0 - (1.0 - d) * (1.0 - s);              // screen
            else if (mode == 2) o = mix (d, s, a);                            // normal
            else if (mode == 3) o = max (d, s);                               // lighten
            else if (mode == 4) o = mix (d, d * s, a);                        // multiply
            else if (mode == 5) o = mix (d, min (d, s), a);                   // darken
            else if (mode == 6) o = mix (d, mix (2.0 * d * s, 1.0 - 2.0 * (1.0 - d) * (1.0 - s), step (vec3 (0.5), d)), a);   // overlay
            else if (mode == 7) o = abs (d - s);                              // difference
            else if (mode == 8) o = d + s - 2.0 * d * s;                      // exclusion
            else if (mode == 9) o = d - s;                                    // subtract

            gl_FragColor = vec4 (clamp (o, 0.0, 1.0), 1.0);
        }
    )";

    constexpr const char* copyFragmentShader = R"(
        uniform sampler2D image;
        varying vec2 uv;
        void main()
        {
            gl_FragColor = vec4 (texture2D (image, uv).rgb, 1.0);
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
// One layer of a scope: the analysis of one source, on a thread of its own,
// and the buffers its picture builds up in. Lives on the GL thread.
class ScopeComponent::LayerRenderer
{
public:
    LayerRenderer (ScopeComponent& ownerToUse, const ScopeSource& sourceToDraw)
        : owner (ownerToUse), source (sourceToDraw)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            block[ch].resize ((size_t) blockSize);
            analytic[ch].resize ((size_t) blockSize);
        }
        timeScratch.resize ((size_t) blockSize);
        freqScratch.resize ((size_t) blockSize);
        thread.startThread();
    }

    ~LayerRenderer()
    {
        thread.signalThreadShouldExit();
        frameWanted.signal();
        thread.stopThread (2000);
        for (auto* target : { &accumTarget, &currentTarget, &blurTempTarget, &blurTarget })
            target->release();
    }

    // Brings accumTarget, currentTarget and blurTarget up to date.
    void render (View, int w, int h, float scale, double dtMs);

    ScopeComponent& owner;
    const ScopeSource& source;
    bool seen = true;                                // still asked for this frame
    Settings shown {};                               // what the last render used
    int blurRadius = 0;
    RenderTarget accumTarget, currentTarget, blurTempTarget, blurTarget;
    std::atomic<float> correlation { 1.0f };

private:
    // One analysed frame: the trace geometry of each panel, in panel
    // coordinates, and the layout it was built for.
    struct Frame
    {
        Layout layout = Layout::sideBySide;
        std::vector<Vertex> panels[2];
    };

    struct AnalysisThread : juce::Thread
    {
        explicit AnalysisThread (LayerRenderer& o) : Thread ("CircleScope analysis"), layer (o) {}
        void run() override { layer.runAnalysis(); }
        LayerRenderer& layer;
    };

    void runAnalysis();
    Range computeRange (const Settings&) const;
    void updateBandWeights (const Settings&);
    void analyse (int channel);
    void updateCorrelation (const Range&);
    void addTrajectory (std::vector<Vertex>&, int channel, const Settings&, const Range&,
                        std::optional<juce::Colour> colourOverride, float halfWidth);
    void addGoniometer (std::vector<Vertex>&, const Settings&, const Range&, float halfWidth);

    // Hand-over of frames from the analysis thread to the GL thread. The GL
    // thread asks for the next frame as it starts drawing the current one.
    Frame working, pending, front;                   // analysis thread, shared (frameLock), GL thread
    juce::SpinLock frameLock;
    bool pendingIsNew = false;
    juce::WaitableEvent frameWanted;
    std::atomic<float> lineHalfWidth { 0.004f };     // set by the GL thread, which knows the size

    // Analysis thread only.
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> block[2];
    std::vector<std::complex<float>> analytic[2];    // z(t) = s(t) + i*H{s}(t)
    std::vector<std::complex<float>> timeScratch, freqScratch;
    std::vector<float> bandWeights;                  // per FFT bin, 0..N/2
    float weightsLowCut = -1.0f, weightsHighCut = -1.0f;
    double weightsSampleRate = 0.0;
    bool bandLimited = false;
    float gain[3] { 1.0f, 1.0f, 1.0f };              // L, R, goniometer
    size_t blockStart[2] {};                         // absolute position of each block's first sample

    AnalysisThread thread { *this };
};

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
ScopeComponent::ScopeComponent (LayerProvider layerProvider, bool labels)
    : provider (std::move (layerProvider)), showLabels (labels)
{
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

ScopeComponent::ScopeComponent (const ScopeSource& source, bool labels)
    : ScopeComponent ([&source] (LayerList& list) { list[0] = { &source, {} }; return 1; }, labels)
{
}

void ScopeComponent::setLayerProvider (LayerProvider layerProvider)
{
    const juce::SpinLock::ScopedLockType lock (providerLock);
    provider = std::move (layerProvider);
}

juce::String ScopeComponent::getRendererName() const
{
    const juce::SpinLock::ScopedLockType lock (rendererNameLock);
    return rendererName;
}

ScopeComponent::~ScopeComponent()
{
    stopTimer();
    context.detach();   // the layers go in openGLContextClosing
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
    const int layout = drawnLayout.load();
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
    blendShader = buildShader (context, fullScreenVertexShader, blendFragmentShader);
    copyShader = buildShader (context, fullScreenVertexShader, copyFragmentShader);
    if (lineShader == nullptr || blurShader == nullptr || blendShader == nullptr || copyShader == nullptr)
    {
        lineShader.reset();
        blurShader.reset();
        blendShader.reset();
        copyShader.reset();
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
    layers.clear();
    for (auto& target : canvas)
        target.release();
    for (auto& target : previewTargets)
        target.release();
    if (vbo != 0) glDeleteBuffers (1, &vbo);
    if (vao != 0) glDeleteVertexArrays (1, &vao);
    vbo = vao = 0;
    lineShader.reset();
    blurShader.reset();
    blendShader.reset();
    copyShader.reset();
}

//==============================================================================
// The settings of a layer, or the defaults where there is none.
ScopeComponent::Settings ScopeComponent::readSettings (const ScopeSource* src)
{
    using namespace scope;
    auto get = [src] (int index) { return src != nullptr ? src->get (index) : paramTable[index].def; };

    // The axis of the Swing mode makes one swing per window length, or
    // swings at its own rate.
    const float swingHz = get (pSwingSync) > 0.5f ? 1000.0f / juce::jmax (1.0f, get (pWindow)) : get (pSwingRate);

    return { (Mode) juce::jlimit (0, 2, (int) get (pMode)),
             get (pColour) > 0.5f,
             get (pGuides) > 0.5f,
             (Layout) juce::jlimit (0, 2, (int) get (pLayout)),
             juce::jlimit (0, 9, (int) get (pBlend)),
             get (pSwingMotion) > 0.5f,
             swingHz,
             get (pWindow),
             get (pDelay),
             get (pAfterglow),
             get (pBrightness) * 0.01f,
             get (pContrast) * 0.01f,
             get (pBlur),
             get (pLowCut),
             get (pHighCut),
             juce::jmax (1000.0, src != nullptr ? src->sampleRate.load() : 48000.0) };
}

ScopeComponent::PanelPlacement ScopeComponent::placePanel (Layout layout, int panel, int w, int h, int margin, View view)
{
    // Lay the panels out on the whole virtual canvas, then shift by where
    // this component's part of it starts. GL's y axis points up.
    const float vw = juce::jmax (1.0e-4f, view.w), vh = juce::jmax (1.0e-4f, view.h);
    const int canvasW = juce::roundToInt ((float) w / vw), canvasH = juce::roundToInt ((float) h / vh);
    const int originX = juce::roundToInt (view.x * (float) canvasW);
    const int originY = juce::roundToInt ((1.0f - view.y - vh) * (float) canvasH);

    const auto b = panelBounds (layout, panel, canvasW, canvasH, margin).translated (-originX, -originY);
    return { (float) b.getWidth() / (float) w,
             (float) b.getHeight() / (float) h,
             2.0f * (float) b.getCentreX() / (float) w - 1.0f,
             2.0f * (float) b.getCentreY() / (float) h - 1.0f,
             b.getIntersection ({ 0, 0, w, h }) };
}

// Half the line thickness (~2.4 px in all), in panel coordinates. It follows
// the canvas, so every tile of a divided picture draws lines equally thick.
float ScopeComponent::lineHalfWidthFor (Layout layout, View view, int w, int h, int margin, float scale)
{
    const int side = panelBounds (layout, 0, juce::roundToInt ((float) w / juce::jmax (1.0e-4f, view.w)),
                                  juce::roundToInt ((float) h / juce::jmax (1.0e-4f, view.h)), margin).getWidth();
    return 1.2f * scale / (float) side;
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

//==============================================================================
// The analysis thread of a layer.
ScopeComponent::Range ScopeComponent::LayerRenderer::computeRange (const Settings& s) const
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
void ScopeComponent::LayerRenderer::updateBandWeights (const Settings& s)
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

void ScopeComponent::LayerRenderer::analyse (int channel)
{
    auto& samples = block[channel];
    blockStart[channel] = source.rings[channel].readLatest (samples.data(), blockSize);

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

void ScopeComponent::LayerRenderer::updateCorrelation (const Range& r)
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
    const float previous = correlation.load();
    correlation.store (previous + (target - previous) * 0.2f);
}

// The analysis thread: one frame of work each time the GL thread asks.
void ScopeComponent::LayerRenderer::runAnalysis()
{
    while (! thread.threadShouldExit())
    {
        if (! frameWanted.wait (100))
            continue;   // not being drawn (hidden, minimised): stay idle

        const auto s = readSettings (&source);
        const auto range = computeRange (s);
        const float halfWidth = lineHalfWidth.load();

        updateBandWeights (s);
        analyse (0);
        analyse (1);
        updateCorrelation (range);

        working.layout = s.layout;
        for (auto& panel : working.panels)
            panel.clear();

        switch (s.layout)
        {
            case Layout::sideBySide:
                addTrajectory (working.panels[0], 0, s, range, std::nullopt, halfWidth);
                addTrajectory (working.panels[1], 1, s, range, std::nullopt, halfWidth);
                break;
            case Layout::overlay:
                addTrajectory (working.panels[0], 0, s, range, leftColour, halfWidth);
                addTrajectory (working.panels[0], 1, s, range, rightColour, halfWidth);
                break;
            case Layout::midSide:
                addGoniometer (working.panels[0], s, range, halfWidth);
                break;
        }

        const juce::SpinLock::ScopedLockType lock (frameLock);
        std::swap (working, pending);
        pendingIsNew = true;
    }
}

void ScopeComponent::LayerRenderer::addTrajectory (std::vector<Vertex>& out, int channel, const Settings& s, const Range& r,
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

    // Swing mode: the axis angle follows the samples' absolute position, so
    // a point keeps its place on screen for as long as it is shown.
    const double swingsPerSample = (double) s.swingHz / s.sampleRate;
    auto swingAngle = [&] (int i)
    {
        const double swings = (double) (blockStart[channel] + (size_t) i) * swingsPerSample;
        const double phase = juce::MathConstants<double>::twoPi * (swings - std::floor (swings));
        return s.swingRound ? phase : juce::MathConstants<double>::pi * std::sin (phase);
    };

    Vertex prev {};
    bool havePrev = false;
    for (int i = r.start; i < r.end; i += r.stride)
    {
        const auto zi = z[(size_t) i];
        float x = zi.real() * g;
        float y = 0.0f;

        switch (s.mode)
        {
            case Mode::quadrature: y = zi.imag() * g; break;
            case Mode::delayEmbed: y = samples[(size_t) (i - r.delay)] * g; break;
            case Mode::swing:
            {
                const double angle = swingAngle (i);
                y = x * (float) std::sin (angle);
                x *= (float) std::cos (angle);
                break;
            }
        }

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
            addSegment (out, prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

void ScopeComponent::LayerRenderer::addGoniometer (std::vector<Vertex>& out, const Settings& s, const Range& r, float halfWidth)
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
            addSegment (out, prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}

//==============================================================================
void ScopeComponent::addSegment (std::vector<Vertex>& out, Vertex a, Vertex b, float halfWidth)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt (dx * dx + dy * dy);
    if (len < 1.0e-6f)
        return;

    const float nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
    auto offset = [] (Vertex v, float ox, float oy) { v.x += ox; v.y += oy; return v; };

    const Vertex a0 = offset (a, nx, ny), a1 = offset (a, -nx, -ny);
    const Vertex b0 = offset (b, nx, ny), b1 = offset (b, -nx, -ny);
    out.insert (out.end(), { a0, a1, b0, b0, a1, b1 });
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

    addSegment (vertices, v (-1.0f, 0.0f), v (1.0f, 0.0f), halfWidth);
    addSegment (vertices, v (0.0f, -1.0f), v (0.0f, 1.0f), halfWidth);

    constexpr int steps = 128;
    for (int i = 0; i < steps; ++i)
    {
        const float a0 = juce::MathConstants<float>::twoPi * (float) i / steps;
        const float a1 = juce::MathConstants<float>::twoPi * (float) (i + 1) / steps;
        addSegment (vertices, v (plotRadius * std::cos (a0), plotRadius * std::sin (a0)),
                    v (plotRadius * std::cos (a1), plotRadius * std::sin (a1)), halfWidth);
    }
}

void ScopeComponent::addGoniometerGuides (float halfWidth, float shown)
{
    const float c = 0.18f;
    auto v = [c] (float x, float y, float k = 1.0f) { return Vertex { x, y, c * k, c * k, c * 1.1f * k, 1.0f }; };

    const float d = plotRadius * 0.7071f;
    addSegment (vertices, v (0.0f, -plotRadius), v (0.0f, plotRadius), halfWidth);           // mono (M)
    addSegment (vertices, v (-plotRadius, 0.0f), v (plotRadius, 0.0f), halfWidth);           // anti-phase (S)
    addSegment (vertices, v (-d, -d, 0.6f), v (d, d, 0.6f), halfWidth);                       // R only
    addSegment (vertices, v (-d, d, 0.6f), v (d, -d, 0.6f), halfWidth);                       // L only

    // Correlation bar: track, centre tick, and a marker coloured by sign.
    const float barHalf = 0.85f;
    addSegment (vertices, v (-barHalf, barY), v (barHalf, barY), halfWidth);
    addSegment (vertices, v (0.0f, barY - 0.02f), v (0.0f, barY + 0.02f), halfWidth);

    const auto markerColour = shown > 0.3f ? juce::Colour (0xff97c459)
                            : shown >= 0.0f ? juce::Colour (0xffef9f27)
                                            : juce::Colour (0xffe24b4a);
    const float mx = barHalf * juce::jlimit (-1.0f, 1.0f, shown);
    const Vertex top { mx, barY + 0.035f, markerColour.getFloatRed(), markerColour.getFloatGreen(), markerColour.getFloatBlue(), 1.0f };
    Vertex bottom = top;
    bottom.y = barY - 0.035f;
    addSegment (vertices, bottom, top, halfWidth * 3.0f);
}

//==============================================================================
void ScopeComponent::drawVertices (const std::vector<Vertex>& toDraw, GLint positionAttrib, GLint colourAttrib)
{
    if (toDraw.empty() || positionAttrib < 0)
        return;

    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (toDraw.size() * sizeof (Vertex)), toDraw.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray ((GLuint) positionAttrib);
    glVertexAttribPointer ((GLuint) positionAttrib, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex), nullptr);
    if (colourAttrib >= 0)
    {
        glEnableVertexAttribArray ((GLuint) colourAttrib);
        glVertexAttribPointer ((GLuint) colourAttrib, 4, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                               reinterpret_cast<void*> (2 * sizeof (float)));
    }

    glDrawArrays (GL_TRIANGLES, 0, (GLsizei) toDraw.size());

    glDisableVertexAttribArray ((GLuint) positionAttrib);
    if (colourAttrib >= 0)
        glDisableVertexAttribArray ((GLuint) colourAttrib);
}

void ScopeComponent::drawFullScreen (const juce::OpenGLShaderProgram& program)
{
    vertices.clear();
    addFullScreenQuad (0.0f, 0.0f, 0.0f, 0.0f);
    drawVertices (vertices, glGetAttribLocation (program.getProgramID(), "position"), -1);
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
void ScopeComponent::LayerRenderer::render (View view, int w, int h, float scale, double dtMs)
{
    // Take the newest analysed frame and ask for the next one, which the
    // analysis thread works out while this one is drawn. If none is ready
    // the previous frame is drawn again.
    {
        const juce::SpinLock::ScopedLockType lock (frameLock);
        if (pendingIsNew)
        {
            std::swap (pending, front);
            pendingIsNew = false;
        }
    }
    frameWanted.signal();

    // The look follows the parameters at once; the layout has to be the one
    // the frame's geometry was built for.
    shown = readSettings (&source);
    shown.layout = front.layout;

    // The afterglow decays by the time since the last frame, so it does not
    // depend on the frame rate.
    const float keep = shown.afterglowMs >= 1.0f ? (float) std::exp (-dtMs / (double) shown.afterglowMs) : 0.0f;
    const int margin = juce::roundToInt ((float) panelMargin * scale);
    lineHalfWidth.store (lineHalfWidthFor (shown.layout, view, w, h, margin, scale));

    accumTarget.ensure (w, h);
    currentTarget.ensure (w, h);
    blurTempTarget.ensure (w / 2, h / 2);
    blurTarget.ensure (w / 2, h / 2);

    // 1. Afterglow decays (dst *= keep); the live-beam buffer starts empty.
    accumTarget.bind();
    owner.useLineShader (1.0f);
    glBlendFunc (GL_ZERO, GL_SRC_ALPHA);
    owner.vertices.clear();
    owner.addFullScreenQuad (0.0f, 0.0f, 0.0f, keep);
    owner.drawVertices (owner.vertices, owner.linePosition, owner.lineColour);

    currentTarget.bind();
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    // 2. This frame's traces: added to the afterglow at (1 - keep) so its
    //    steady state matches "off", and to the live beam at full strength
    //    so a shape that just moved is never dim.
    glBlendFunc (GL_SRC_ALPHA, GL_ONE);
    glEnable (GL_SCISSOR_TEST);
    for (int panel = 0; panel < numPanels (shown.layout); ++panel)
    {
        const auto placement = placePanel (shown.layout, panel, w, h, margin, view);

        for (auto* target : { &accumTarget, &currentTarget })
        {
            target->bind();
            owner.useLineShader (target == &accumTarget ? 1.0f - keep : 1.0f);
            if (owner.usePanel (placement))
                owner.drawVertices (front.panels[panel], owner.linePosition, owner.lineColour);
        }
    }
    glDisable (GL_SCISSOR_TEST);

    // 3. Gaussian blur of (afterglow + beam) at half resolution, two passes.
    blurRadius = juce::jlimit (0, maxBlurRadius, juce::roundToInt (shown.blurPx * scale * 0.5f));
    if (blurRadius > 0)
    {
        auto& shader = *owner.blurShader;
        glDisable (GL_BLEND);
        shader.use();
        shader.setUniform ("sourceA", (GLint) 0);
        shader.setUniform ("sourceB", (GLint) 1);
        shader.setUniform ("sigma", juce::jmax (0.5f, (float) blurRadius / 2.5f));
        shader.setUniform ("radius", (GLint) blurRadius);

        blurTempTarget.bind();
        bindTexture (0, accumTarget.texture);
        bindTexture (1, currentTarget.texture);
        shader.setUniform ("mixB", 1.0f);
        shader.setUniform ("texelStep", 1.0f / (float) blurTempTarget.width, 0.0f);
        owner.drawFullScreen (shader);

        blurTarget.bind();
        bindTexture (0, blurTempTarget.texture);
        bindTexture (1, blurTempTarget.texture);
        shader.setUniform ("mixB", 0.0f);
        shader.setUniform ("texelStep", 0.0f, 1.0f / (float) blurTarget.height);
        owner.drawFullScreen (shader);
        glEnable (GL_BLEND);
    }
}

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

    // What to draw this frame, bottom layer first.
    LayerList list;
    int count = 0;
    {
        const juce::SpinLock::ScopedLockType lock (providerLock);
        if (provider != nullptr)
            count = juce::jlimit (0, maxLayers, provider (list));
    }

    // Each layer keeps its renderer for as long as it is asked for, so its
    // afterglow and auto-gain carry on from frame to frame.
    for (auto& layer : layers)
        layer->seen = false;

    LayerRenderer* drawn[maxLayers] {};
    for (int i = 0; i < count; ++i)
    {
        auto found = std::find_if (layers.begin(), layers.end(),
                                   [&] (const auto& layer) { return &layer->source == list[(size_t) i].source && ! layer->seen; });
        if (found == layers.end())
        {
            layers.push_back (std::make_unique<LayerRenderer> (*this, *list[(size_t) i].source));
            found = layers.end() - 1;
        }
        (*found)->seen = true;
        drawn[i] = found->get();
    }
    layers.erase (std::remove_if (layers.begin(), layers.end(), [] (const auto& layer) { return ! layer->seen; }), layers.end());

    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastFrameMs > 0.0 ? juce::jlimit (1.0, 100.0, now - lastFrameMs) : 16.0;
    lastFrameMs = now;

    if (vao != 0)
        glBindVertexArray (vao);
    glEnable (GL_BLEND);

    for (int i = 0; i < count; ++i)
        drawn[i]->render (list[(size_t) i].view, w, h, scale, dt);

    // The canvas starts as the background with the guides, which belong to
    // the bottom layer and are not affected by any image control.
    auto base = readSettings (nullptr);
    View baseView;
    if (count > 0)
    {
        base = drawn[0]->shown;
        baseView = list[0].view;
    }
    drawnLayout.store ((int) base.layout);

    canvas[0].ensure (w, h);
    canvas[1].ensure (w, h);
    canvas[0].bind();
    glClearColor (backgroundColour.getFloatRed(), backgroundColour.getFloatGreen(), backgroundColour.getFloatBlue(), 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (base.guides)
    {
        const int margin = juce::roundToInt ((float) panelMargin * scale);
        const float halfWidth = 0.5f * lineHalfWidthFor (base.layout, baseView, w, h, margin, scale);

        glBlendFunc (GL_SRC_ALPHA, GL_ONE);
        useLineShader (1.0f);
        glEnable (GL_SCISSOR_TEST);
        for (int panel = 0; panel < numPanels (base.layout); ++panel)
        {
            if (! usePanel (placePanel (base.layout, panel, w, h, margin, baseView)))
                continue;

            vertices.clear();
            if (base.layout == Layout::midSide)
                addGoniometerGuides (halfWidth, count > 0 ? drawn[0]->correlation.load() : 1.0f);
            else
                addTrajectoryGuides (halfWidth);
            drawVertices (vertices, linePosition, lineColour);
        }
        glDisable (GL_SCISSOR_TEST);
    }

    // Each layer in turn is combined with the canvas so far into the other
    // canvas buffer: a blend mode has to read what is under it.
    glDisable (GL_BLEND);
    blendShader->use();
    blendShader->setUniform ("below", (GLint) 0);
    blendShader->setUniform ("accumulation", (GLint) 1);
    blendShader->setUniform ("current", (GLint) 2);
    blendShader->setUniform ("halo", (GLint) 3);

    int top = 0;
    for (int i = 0; i < count; ++i)
    {
        const auto& layer = *drawn[i];
        canvas[1 - top].bind();
        bindTexture (0, canvas[top].texture);
        bindTexture (1, layer.accumTarget.texture);
        bindTexture (2, layer.currentTarget.texture);
        bindTexture (3, layer.blurTarget.texture);
        blendShader->setUniform ("exposure", baseExposure * layer.shown.brightness);
        blendShader->setUniform ("contrast", layer.shown.contrast);
        blendShader->setUniform ("haloGain", layer.blurRadius > 0 ? haloGain : 0.0f);
        blendShader->setUniform ("mode", (GLint) layer.shown.blend);
        drawFullScreen (*blendShader);
        top = 1 - top;
    }

    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) screenFbo);
    glViewport (0, 0, w, h);
    copyShader->use();
    copyShader->setUniform ("image", (GLint) 0);
    bindTexture (0, canvas[top].texture);
    drawFullScreen (*copyShader);

    for (int unit = 3; unit >= 0; --unit)
        bindTexture (unit, 0);
    glEnable (GL_BLEND);

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
