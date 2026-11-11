#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <complex>
#include <optional>
#include "PluginProcessor.h"

// Draws the stereo signal as 2D trajectories.
//
// Per-channel trajectory (Side by side / Overlay layouts):
//   Quadrature:  x = s(t), y = H{s}(t) (90 degrees shifted, via the analytic
//                signal). A pure sine traces a perfect circle.
//   Delay embed: x = s(t), y = s(t - delay).
//
// Mid / side layout: a goniometer. Vertical = mid (L+R), horizontal = side
// (R-L). Mono is a vertical line, out-of-phase a horizontal one. A
// correlation bar runs along the bottom (-1 to +1).
//
// Image pipeline (all half-float render targets):
//   accum   : afterglow; decays every frame, gets this frame's traces added
//   current : this frame's traces only, at full strength (the "beam")
//   blur    : accum + current, Gaussian blurred (separable, half resolution)
//   screen  : guides + tone map (brightness, contrast) of
//             accum + current + blur halo
class ScopeComponent : public juce::Component,
                       private juce::OpenGLRenderer,
                       private juce::Timer
{
public:
    explicit ScopeComponent (CircleScopeProcessor&);
    ~ScopeComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Vertex { float x, y, r, g, b, a; };
    enum class Layout { sideBySide = 0, overlay = 1, midSide = 2 };

    struct Settings
    {
        bool delayMode, fixedColour;
        Layout layout;
        float windowMs, delayMs, afterglowMs;
        float brightness, contrast, blurPx;
        double sampleRate;
    };

    // Sample range of the analysis block that gets drawn this frame.
    struct Range { int start, end, length, delay, stride, freqLag; };

    // Offscreen half-float colour buffer.
    struct RenderTarget
    {
        GLuint fbo = 0, texture = 0;
        int width = 0, height = 0;

        void ensure (int w, int h);
        void release();
        void bind() const;
    };

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;

    Settings readSettings() const;
    Range computeRange (const Settings&) const;
    void analyse (int channel);
    void updateCorrelation (const Range&);

    void addTrajectory (int channel, const Settings&, const Range&, std::optional<juce::Colour> colourOverride, float halfWidth);
    void addGoniometer (const Settings&, const Range&, float halfWidth);
    void addTrajectoryGuides (float halfWidth);
    void addGoniometerGuides (float halfWidth);
    void addSegment (Vertex a, Vertex b, float halfWidth);
    void addFullScreenQuad (float r, float g, float b, float a);
    void drawVertices (GLint positionAttrib, GLint colourAttrib);
    void drawFullScreen (const juce::OpenGLShaderProgram&);
    void useLineShader (float alphaScale);

    static int numPanels (Layout);
    static juce::Rectangle<int> panelBounds (Layout, int panel, int w, int h, int margin);
    static juce::Colour colourForFrequency (double hz);

    CircleScopeProcessor& processor;
    juce::OpenGLContext context;

    std::unique_ptr<juce::OpenGLShaderProgram> lineShader, blurShader, compositeShader;
    GLint linePosition = -1, lineColour = -1;
    GLuint vbo = 0, vao = 0;

    RenderTarget accumTarget, currentTarget, blurTempTarget, blurTarget;
    double lastFrameMs = 0.0;

    std::atomic<int> width { 0 }, height { 0 }, shownLayout { -1 };

    // Analysis buffers (render thread only).
    static constexpr int fftOrder = 14;
    static constexpr int blockSize = 1 << fftOrder;   // 16384 samples
    static constexpr int edgeMargin = 1024;          // skip FFT edge artefacts
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> block[2];
    std::vector<std::complex<float>> analytic[2];    // z(t) = s(t) + i*H{s}(t)
    std::vector<std::complex<float>> timeScratch, freqScratch;
    std::vector<Vertex> vertices;
    float gain[3] { 1.0f, 1.0f, 1.0f };              // L, R, goniometer
    float correlation = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeComponent)
};
