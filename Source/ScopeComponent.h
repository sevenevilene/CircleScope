#pragma once

#include <juce_opengl/juce_opengl.h>
#include <array>
#include "ScopeCurve.h"

// Draws finished frames. A scope does no analysis and sees no audio: every
// layer is a CurveFeed, which hands it the newest frame of a drawing (the
// points of its curves, in 3D) and the parameters that say how to show it.
// The plugin's own scopes draw its analyser's feed; a wall or a cluster
// node draws the feeds it receives from the network, one per plugin
// instance sending to the stream shown. So all of them show the same thing.
//
// The layers are combined bottom to top by the upper layer's blend mode.
// One camera looks at the whole scene (Plane.h): the view of the lowest
// layer that has turned it.
//
// A layer normally shows its whole picture. Its View makes it show only a
// part of a larger virtual canvas instead: that is how one picture is tiled
// over the screens of a video wall or the cells of a grid.
//
// A frame is curves, drawn as lines, or a field of standing waves (the
// Field mode), which is drawn here from its sources: every plane in it is
// a large sheet, and the fragment shader works out each pixel of it from
// where the pixel is along every source's axes. Or, as a volume, the
// shader walks along each pixel's line of sight through the field and adds
// up what it meets, the near hiding the far. With the lens open
// (Field aperture) its steps look through different parts of a lens, which
// blurs what is off the depth in focus.
//
// Image pipeline (all half-float render targets):
//   per layer
//     accum   : afterglow; decays every frame, gets this frame's traces added
//     current : this frame's traces only, at full strength (the "beam")
//     blur    : accum + current, Gaussian blurred (separable, half resolution)
//   canvas  : background + the guides of every plane in use, then every
//             layer's tone map (brightness, contrast) of accum + current +
//             blur halo, combined with what is under it by its blend mode
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
        const scope::CurveFeed* feed = nullptr;   // must outlive the scope
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
    ScopeComponent (const scope::CurveFeed&, bool showLabels);   // one layer, the whole picture
    ~ScopeComponent() override;

    void setLayerProvider (LayerProvider);

    // The side the scene is shown from here (see Plane.h).
    void setPerspective (scope::Perspective from) { perspective.store ((int) from); }

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
    using Vertex = scope::Vertex;
    using Layout = scope::Layout;

    // How a layer is shown: the part of its parameters a scope uses.
    struct Look
    {
        int blend;                 // scope::Blend
        float afterglowMs, brightness, contrast, blurPx;
        float viewYaw, viewPitch, viewRoll;
        scope::Vec3 cameraAt;      // the point of the scene the camera looks at
        bool fieldVolume;          // Field mode: the whole volume, not only the planes' cuts
        float fieldDensity;        // and how much a plane unit of full field hides what is behind it
        float fieldFocus;          // the depth that is sharp, towards the viewer from the centre of the scene
        float fieldAperture;       // 0..1: how much the rest blurs; 0 is everything sharp
        float alpha;               // 0..1: the alpha of the colours, in every mode
        bool fieldInterfere;       // the sources' waves are added before they are coloured
        float fieldPure;           // 0..1: how pure the field's colours are
    };

    // Where one panel lands in this component's framebuffer: a transform from
    // panel coordinates (-1..1) to clip space, and the pixels it may touch.
    struct PanelPlacement
    {
        float scaleX, scaleY, offsetX, offsetY;
        juce::Rectangle<int> scissor;
    };

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

    bool usePanel (const PanelPlacement&, const scope::Mat3& orient, float halfWidth, scope::Vec3 shift = {});
    void capturePreview (GLuint screenFbo, int w, int h);
    void addTrajectoryGuides();
    void addGoniometerGuides (float correlation);
    void addFullScreenQuad (float r, float g, float b, float a);
    void drawVertices (const std::vector<Vertex>&, GLint positionAttrib, GLint colourAttrib);
    void drawFullScreen (const juce::OpenGLShaderProgram&);
    void useLineShader (float alphaScale);
    void setLineTransform (float scaleX, float scaleY, float offsetX, float offsetY);

    static Look readLook (const ScopeParamSet*);
    static scope::Camera cameraOf (const Look&);
    static PanelPlacement placePanel (Layout, int panel, int w, int h, int margin, View, bool flat);
    static float lineHalfWidthFor (Layout, View, int w, int h, int margin, float scale);
    static int numPanels (Layout);
    static juce::Rectangle<int> panelBounds (Layout, int panel, int w, int h, int margin);

    LayerProvider provider;
    juce::SpinLock providerLock;
    const bool showLabels;
    juce::OpenGLContext context;

    std::unique_ptr<juce::OpenGLShaderProgram> lineShader, fieldShader, volumeShader, blurShader, blendShader, copyShader;
    GLint linePosition = -1, lineColour = -1, lineOther = -1, lineDepth = -1;
    GLuint vbo = 0, vao = 0;

    // GL thread.
    std::vector<std::unique_ptr<LayerRenderer>> layers;
    RenderTarget canvas[2];                          // the layers so far, and the next one's result
    std::vector<Vertex> vertices;                    // scratch
    RenderTarget previewTargets[4];                  // successive halvings of the screen
    std::vector<juce::uint8> previewPixels;
    std::atomic<int> previewIntervalMs { 0 };
    std::atomic<int> perspective { 0 };              // scope::Perspective
    double lastFrameMs = 0.0, lastPreviewMs = 0.0, fpsWindowStartMs = 0.0;
    int framesInWindow = 0;
    std::atomic<float> framesPerSecond { 0.0f };

    juce::SpinLock rendererNameLock;
    juce::String rendererName;

    // drawnLayout: the bottom layer's (-1 when it is not one flat picture, so the labels would not fit), set by the GL thread. shownLayout: what the labels were painted for.
    std::atomic<int> width { 0 }, height { 0 }, drawnLayout { 0 }, shownLayout { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeComponent)
};
