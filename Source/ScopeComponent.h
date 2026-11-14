#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <complex>
#include <optional>
#include "ScopeSource.h"

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
// Low cut / High cut band-limit the signal in the frequency domain before
// any of that, so several scopes can each show one band of the same audio.
//
// A scope normally shows the whole picture. setView() makes it show only a
// part of a larger virtual canvas instead: that is how one picture is tiled
// over the screens of a video wall, each node drawing its own cell.
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
    // showLabels: paint the small text labels (L, R, M, S) over the picture.
    // Off gives a clean image and skips JUCE's component painting entirely.
    ScopeComponent (ScopeSource&, bool showLabels);
    ~ScopeComponent() override;

    // Part of the virtual canvas shown here, as fractions of it (y from the top).
    struct View { float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f; };
    void setView (View);

    // Switch to another source. It must outlive this component.
    void setSource (ScopeSource&);

    // Small copies of the finished frame, for remote monitoring. The callback
    // runs on the render thread. An interval of 0 turns previews off.
    std::function<void (const juce::Image&)> onPreview;
    void setPreviewIntervalMs (int ms) { previewIntervalMs.store (ms); }

    float getFramesPerSecond() const noexcept { return framesPerSecond.load(); }
    juce::String getRendererName() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    std::function<void()> onDoubleClick;

private:
    struct Vertex { float x, y, r, g, b, a; };
    enum class Layout { sideBySide = 0, overlay = 1, midSide = 2 };

    struct Settings
    {
        bool delayMode, fixedColour, guides;
        Layout layout;
        float windowMs, delayMs, afterglowMs;
        float brightness, contrast, blurPx;
        float lowCutHz, highCutHz;
        double sampleRate;
    };

    // Where one panel lands in this component's framebuffer: a transform from
    // panel coordinates (-1..1) to clip space, and the pixels it may touch.
    struct PanelPlacement
    {
        float scaleX, scaleY, offsetX, offsetY;
        juce::Rectangle<int> scissor;
    };

    // Sample range of the analysis block that gets drawn this frame.
    struct Range { int start, end, length, delay, stride, freqLag; };

    // Offscreen half-float colour buffer.
    struct RenderTarget
    {
        GLuint fbo = 0, texture = 0;
        int width = 0, height = 0;

        void ensure (int w, int h, bool halfFloat = true);
        void release();
        void bind() const;
    };

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;

    Settings readSettings() const;
    Range computeRange (const Settings&) const;
    void analyse (int channel, const Settings&);
    void updateBandWeights (const Settings&);
    PanelPlacement placePanel (Layout, int panel, int w, int h, int margin) const;
    bool usePanel (const PanelPlacement&);
    void capturePreview (GLuint screenFbo, int w, int h);
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
    void setLineTransform (float scaleX, float scaleY, float offsetX, float offsetY);

    static int numPanels (Layout);
    static juce::Rectangle<int> panelBounds (Layout, int panel, int w, int h, int margin);
    static juce::Colour colourForFrequency (double hz);

    std::atomic<ScopeSource*> source;
    const bool showLabels;
    juce::OpenGLContext context;

    std::unique_ptr<juce::OpenGLShaderProgram> lineShader, blurShader, compositeShader;
    GLint linePosition = -1, lineColour = -1;
    GLuint vbo = 0, vao = 0;

    RenderTarget accumTarget, currentTarget, blurTempTarget, blurTarget;
    RenderTarget previewTargets[4];                  // successive halvings of the screen
    std::vector<juce::uint8> previewPixels;
    std::atomic<int> previewIntervalMs { 0 };
    double lastFrameMs = 0.0, lastPreviewMs = 0.0, fpsWindowStartMs = 0.0;
    int framesInWindow = 0;
    std::atomic<float> framesPerSecond { 0.0f };
    std::atomic<float> viewX { 0.0f }, viewY { 0.0f }, viewW { 1.0f }, viewH { 1.0f };

    juce::SpinLock rendererNameLock;
    juce::String rendererName;

    std::atomic<int> width { 0 }, height { 0 }, shownLayout { -1 };

    // Analysis buffers (render thread only).
    static constexpr int fftOrder = 14;
    static constexpr int blockSize = 1 << fftOrder;   // 16384 samples
    static constexpr int edgeMargin = 1024;          // skip FFT edge artefacts
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> block[2];
    std::vector<std::complex<float>> analytic[2];    // z(t) = s(t) + i*H{s}(t)
    std::vector<std::complex<float>> timeScratch, freqScratch;
    std::vector<float> bandWeights;                  // per FFT bin, 0..N/2
    float weightsLowCut = -1.0f, weightsHighCut = -1.0f;
    double weightsSampleRate = 0.0;
    bool bandLimited = false;
    std::vector<Vertex> vertices;
    float gain[3] { 1.0f, 1.0f, 1.0f };              // L, R, goniometer
    float correlation = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeComponent)
};
