#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <complex>
#include <optional>
#include "ScopeSource.h"

// Draws the stereo signal as 2D trajectories.
//
// Per-channel trajectory (Side by side / Overlay layouts):
//   Quadrature:  x = s(t), y = H{s}(t) (90 degrees shifted, via the analytic
//                signal). A pure sine traces a perfect circle.
//   Delay embed: x = s(t), y = s(t - delay).
//   Swing:       the signal moves a point along an axis through the centre,
//                and the axis swings round in the plane, so the point is at
//                s(t) * (cos a(t), sin a(t)): a polar plot. The axis either
//                swings back and forth like a pendulum, 180 degrees each
//                way, or turns round and round, once per Window length or
//                at a free rate. A rate tied to the pitch gives a still
//                figure (a rose); slightly off, it slowly turns.
//
// Mid / side layout: a goniometer. Vertical = mid (L+R), horizontal = side
// (R-L). Mono is a vertical line, out-of-phase a horizontal one. A
// correlation bar runs along the bottom (-1 to +1).
//
// Low cut / High cut band-limit the signal in the frequency domain before
// any of that, so several scopes can each show one band of the same audio.
//
// A scope draws one or more layers, each from a source of its own (its
// samples and its settings), combined bottom to top by the upper layer's
// blend mode. The plugin's own scopes have one layer; on a wall or a cluster
// node every plugin instance sending to the stream shown is a layer.
//
// A layer normally shows its whole picture. Its View makes it show only a
// part of a larger virtual canvas instead: that is how one picture is tiled
// over the screens of a video wall or the cells of a grid.
//
// Threads: every layer has its own analysis thread, which reads the samples,
// does the FFT work and builds the trace geometry for the next frame. The GL
// thread (JUCE shares one between all scopes) only draws the latest finished
// frame, so a grid of scopes spreads its heavy work over the CPU cores.
//
// Image pipeline (all half-float render targets):
//   per layer
//     accum   : afterglow; decays every frame, gets this frame's traces added
//     current : this frame's traces only, at full strength (the "beam")
//     blur    : accum + current, Gaussian blurred (separable, half resolution)
//   canvas  : background + the bottom layer's guides, then every layer's
//             tone map (brightness, contrast) of accum + current + blur halo,
//             combined with what is under it by its blend mode
//   screen  : the canvas
class ScopeComponent : public juce::Component,
                       private juce::OpenGLRenderer,
                       private juce::Timer
{
public:
    // Part of a layer's virtual canvas shown here, as fractions of it (y from the top).
    struct View { float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f; };

    struct Layer
    {
        const ScopeSource* source = nullptr;   // must outlive the scope
        View view;
    };

    static constexpr int maxLayers = 8;
    using LayerList = std::array<Layer, maxLayers>;

    // Fills in what to draw, bottom layer first, and returns how many layers
    // that is. Called on the GL thread for every frame.
    using LayerProvider = std::function<int (LayerList&)>;

    // showLabels: paint the small text labels (L, R, M, S) over the picture.
    // Off gives a clean image and skips JUCE's component painting entirely.
    ScopeComponent (LayerProvider, bool showLabels);
    ScopeComponent (const ScopeSource&, bool showLabels);   // one layer, the whole picture
    ~ScopeComponent() override;

    void setLayerProvider (LayerProvider);

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
    using Mode = scope::Mode;

    struct Settings
    {
        scope::Mode mode;
        bool fixedColour, guides;
        Layout layout;
        int blend;                 // scope::Blend
        bool swingRound;
        float swingHz;
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

    class LayerRenderer;

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;

    bool usePanel (const PanelPlacement&);
    void capturePreview (GLuint screenFbo, int w, int h);
    void addTrajectoryGuides (float halfWidth);
    void addGoniometerGuides (float halfWidth, float correlation);
    void addFullScreenQuad (float r, float g, float b, float a);
    void drawVertices (const std::vector<Vertex>&, GLint positionAttrib, GLint colourAttrib);
    void drawFullScreen (const juce::OpenGLShaderProgram&);
    void useLineShader (float alphaScale);
    void setLineTransform (float scaleX, float scaleY, float offsetX, float offsetY);

    static Settings readSettings (const ScopeSource*);
    static PanelPlacement placePanel (Layout, int panel, int w, int h, int margin, View);
    static float lineHalfWidthFor (Layout, View, int w, int h, int margin, float scale);
    static void addSegment (std::vector<Vertex>&, Vertex a, Vertex b, float halfWidth);
    static int numPanels (Layout);
    static juce::Rectangle<int> panelBounds (Layout, int panel, int w, int h, int margin);
    static juce::Colour colourForFrequency (double hz);

    static constexpr int fftOrder = 14;
    static constexpr int blockSize = 1 << fftOrder;   // 16384 samples
    static constexpr int edgeMargin = 1024;          // skip FFT edge artefacts

    LayerProvider provider;
    juce::SpinLock providerLock;
    const bool showLabels;
    juce::OpenGLContext context;

    std::unique_ptr<juce::OpenGLShaderProgram> lineShader, blurShader, blendShader, copyShader;
    GLint linePosition = -1, lineColour = -1;
    GLuint vbo = 0, vao = 0;

    // GL thread.
    std::vector<std::unique_ptr<LayerRenderer>> layers;
    RenderTarget canvas[2];                          // the layers so far, and the next one's result
    std::vector<Vertex> vertices;                    // scratch
    RenderTarget previewTargets[4];                  // successive halvings of the screen
    std::vector<juce::uint8> previewPixels;
    std::atomic<int> previewIntervalMs { 0 };
    double lastFrameMs = 0.0, lastPreviewMs = 0.0, fpsWindowStartMs = 0.0;
    int framesInWindow = 0;
    std::atomic<float> framesPerSecond { 0.0f };

    juce::SpinLock rendererNameLock;
    juce::String rendererName;

    // drawnLayout: the bottom layer's, set by the GL thread. shownLayout: what the labels were painted for.
    std::atomic<int> width { 0 }, height { 0 }, drawnLayout { 0 }, shownLayout { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeComponent)
};
