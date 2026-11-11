#include "ScopeComponent.h"

using namespace juce::gl;

namespace
{
    constexpr const char* vertexShaderSource = R"(
        attribute vec2 position;
        attribute vec4 colour;
        varying vec4 vColour;
        void main()
        {
            vColour = colour;
            gl_Position = vec4 (position, 0.0, 1.0);
        }
    )";

    constexpr const char* fragmentShaderSource = R"(
        varying vec4 vColour;
        void main()
        {
            gl_FragColor = vColour;
        }
    )";

    constexpr float plotRadius = 0.9f;     // where a full-scale pure tone lands
    constexpr int maxPointsPerChannel = 4096;
}

ScopeComponent::ScopeComponent (CircleScopeProcessor& p)
    : processor (p),
      block ((size_t) blockSize),
      spectrum ((size_t) blockSize),
      analytic ((size_t) blockSize)
{
    setOpaque (true);
    context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    context.setRenderer (this);
    context.setContinuousRepainting (true);
    context.setComponentPaintingEnabled (true);   // for the L / R labels
    context.attachTo (*this);
}

ScopeComponent::~ScopeComponent()
{
    context.detach();
}

void ScopeComponent::paint (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (14.0f);
    const auto half = getLocalBounds().withWidth (getWidth() / 2).reduced (12);
    g.drawText ("L", half, juce::Justification::topLeft);
    g.drawText ("R", half.translated (getWidth() / 2, 0), juce::Justification::topLeft);
}

void ScopeComponent::resized()
{
    width.store (getWidth());
    height.store (getHeight());
}

void ScopeComponent::newOpenGLContextCreated()
{
    shader = std::make_unique<juce::OpenGLShaderProgram> (context);
    const bool ok = shader->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vertexShaderSource))
                 && shader->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShaderSource))
                 && shader->link();
    if (! ok)
    {
        DBG ("Shader error: " << shader->getLastError());
        shader.reset();
        return;
    }

    positionAttrib = glGetAttribLocation (shader->getProgramID(), "position");
    colourAttrib = glGetAttribLocation (shader->getProgramID(), "colour");

    glGenVertexArrays (1, &vao);
    glGenBuffers (1, &vbo);
}

void ScopeComponent::openGLContextClosing()
{
    if (vbo != 0) glDeleteBuffers (1, &vbo);
    if (vao != 0) glDeleteVertexArrays (1, &vao);
    vbo = vao = 0;
    shader.reset();
}

void ScopeComponent::renderOpenGL()
{
    const float scale = (float) context.getRenderingScale();
    const int w = juce::roundToInt ((float) width.load() * scale);
    const int h = juce::roundToInt ((float) height.load() * scale);

    glViewport (0, 0, w, h);
    glClearColor (0.035f, 0.035f, 0.045f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (shader == nullptr || w <= 0 || h <= 0)
        return;

    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE);   // additive: overlapping passes glow brighter

    shader->use();
    glBindVertexArray (vao);
    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray ((GLuint) positionAttrib);
    glEnableVertexAttribArray ((GLuint) colourAttrib);
    glVertexAttribPointer ((GLuint) positionAttrib, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex), nullptr);
    glVertexAttribPointer ((GLuint) colourAttrib, 4, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                           reinterpret_cast<void*> (2 * sizeof (float)));

    const int halfW = w / 2;
    const int side = juce::jmax (1, juce::jmin (halfW, h) - juce::roundToInt (16.0f * scale));
    const float lineHalfWidth = 1.2f * scale / (float) side;   // ~2.4 px thick, in NDC

    for (int ch = 0; ch < 2; ++ch)
    {
        vertices.clear();
        addGuides (lineHalfWidth * 0.5f);
        buildChannel (ch, lineHalfWidth);

        glViewport (ch * halfW + (halfW - side) / 2, (h - side) / 2, side, side);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (vertices.size() * sizeof (Vertex)),
                      vertices.data(), GL_STREAM_DRAW);
        glDrawArrays (GL_TRIANGLES, 0, (GLsizei) vertices.size());
    }

    glDisableVertexAttribArray ((GLuint) positionAttrib);
    glDisableVertexAttribArray ((GLuint) colourAttrib);
    glBindVertexArray (0);
}

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

void ScopeComponent::addGuides (float halfWidth)
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

void ScopeComponent::buildChannel (int channel, float halfWidth)
{
    auto& apvts = processor.apvts;
    const bool delayMode = apvts.getRawParameterValue ("mode")->load() > 0.5f;
    const bool fixedColour = apvts.getRawParameterValue ("colour")->load() > 0.5f;
    const float windowMs = apvts.getRawParameterValue ("window")->load();
    const float delayMs = apvts.getRawParameterValue ("delay")->load();
    const double sr = processor.currentSampleRate.load();

    processor.rings[channel].readLatest (block.data(), blockSize);

    // Analytic signal z = s + i*H{s}: zero the negative frequencies, double the positive.
    for (int i = 0; i < blockSize; ++i)
        spectrum[(size_t) i] = { block[(size_t) i], 0.0f };
    fft.perform (spectrum.data(), analytic.data(), false);
    for (int k = 1; k < blockSize / 2; ++k)
        analytic[(size_t) k] *= 2.0f;
    for (int k = blockSize / 2 + 1; k < blockSize; ++k)
        analytic[(size_t) k] = 0.0f;
    fft.perform (analytic.data(), spectrum.data(), true);   // spectrum now holds z(t)

    const int end = blockSize - edgeMargin;
    const int windowLen = juce::jlimit (16, blockSize - 2 * edgeMargin, (int) (windowMs * 0.001 * sr));
    const int start = end - windowLen;
    const int delay = juce::jlimit (1, start, (int) (delayMs * 0.001 * sr));
    const int stride = juce::jmax (1, windowLen / maxPointsPerChannel);
    const int freqLag = juce::jmax (stride, 8);

    // Auto-gain: snap down on loud input, ease back up when it gets quieter.
    float peak = 0.0f;
    for (int i = start; i < end; ++i)
        peak = juce::jmax (peak, std::abs (spectrum[(size_t) i]));
    const float target = plotRadius / juce::jmax (peak, 0.01f);
    gain[channel] = target < gain[channel] ? target : gain[channel] + (target - gain[channel]) * 0.05f;
    const float g = gain[channel];

    const juce::Colour fixed = channel == 0 ? juce::Colour (0xff1d9e75) : juce::Colour (0xffd85a30);

    Vertex prev {};
    bool havePrev = false;
    for (int i = start; i < end; i += stride)
    {
        const auto z = spectrum[(size_t) i];
        const float x = z.real() * g;
        const float y = delayMode ? block[(size_t) (i - delay)] * g : z.imag() * g;

        juce::Colour col = fixed;
        if (! fixedColour)
        {
            // Instantaneous frequency from the phase advance over freqLag samples.
            const auto rot = z * std::conj (spectrum[(size_t) (i - freqLag)]);
            const double hz = std::abs (std::arg (rot)) * sr / (juce::MathConstants<double>::twoPi * freqLag);
            const float u = (float) juce::jlimit (0.0, 1.0, std::log (juce::jmax (hz, 1.0) / 20.0) / std::log (1000.0));
            col = juce::Colour::fromHSV (0.66f * (1.0f - u), 0.8f, 1.0f, 1.0f);   // 20 Hz blue -> 20 kHz red
        }

        const float age = (float) (i - start) / (float) windowLen;
        const Vertex v { x, y, col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(),
                         0.04f + 0.86f * age * age };
        if (havePrev)
            addSegment (prev, v, halfWidth);
        prev = v;
        havePrev = true;
    }
}
