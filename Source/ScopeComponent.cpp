#include "ScopeComponent.h"

using namespace juce::gl;

namespace
{
    // Points arrive in the scene's coordinates (a layer's curve) or a plane's
    // (its guides, at depth 0). They go through the camera, or the plane as
    // the camera sees it, and the perspective, which leaves
    // them in panel coordinates (square, so the same size both ways), and
    // from there to the screen. A corner of a line also knows the segment's
    // other end, and steps sideways from the line as it runs on screen, so a
    // line is equally thick however its plane is turned, and as thick as
    // suits this screen whoever made the points.
    constexpr const char* lineVertexShader = R"(
        attribute vec2 position;
        attribute vec3 other;     // xy: the segment's other end, z: which side, times this line's relative thickness
        attribute vec2 depth;     // z of this point and of the other end
        attribute vec4 colour;
        uniform mat3 orient;      // the points' space (the scene, or a plane) to the viewer's
        uniform vec3 shift;       // and where its centre is for the viewer
        uniform float eye;        // distance of the eye from the panel's centre
        uniform float halfWidth;  // half a line's thickness, in panel units
        uniform vec4 transform;   // panel to clip space: xy scale, zw offset
        varying vec4 vColour;

        vec2 project (vec3 q)
        {
            return q.xy * (eye / (eye - q.z));
        }

        void main()
        {
            vColour = colour;
            vec3 a = orient * vec3 (position, depth.x) + shift;
            vec3 b = orient * vec3 (other.xy, depth.y) + shift;

            // The camera can stand among the lines. What is behind the eye
            // is left out, and a segment that passes it is cut off just in
            // front of it.
            float limit = eye - 0.05;
            if (a.z > limit && b.z > limit)
            {
                gl_Position = vec4 (2.0, 2.0, 2.0, 1.0);
                return;
            }
            if (a.z > limit)
                a = mix (b, a, (limit - b.z) / (a.z - b.z));
            else if (b.z > limit)
                b = mix (a, b, (limit - a.z) / (b.z - a.z));

            vec2 here = project (a);
            vec2 along = project (b) - here;
            float len = length (along);
            vec2 across = len > 1.0e-6 ? vec2 (-along.y, along.x) / len : vec2 (0.0, 1.0);
            gl_Position = vec4 ((here + across * other.z * halfWidth) * transform.xy + transform.zw, 0.0, 1.0);
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

    // The Field mode. A plane of the scene is drawn as one large sheet, a
    // cut through the field; the
    // vertex shader says where in the scene each corner is, and that is
    // handed on to every pixel. The perspective is left to the graphics
    // card (w), so a sheet that passes behind the eye is cut off properly.
    constexpr const char* fieldVertexShader = R"(
        attribute vec2 position;  // on the plane
        uniform mat3 plane;       // the plane to the scene
        uniform vec3 origin;      // the plane's centre in the scene
        uniform mat3 camera;      // the scene to the viewer's space:
        uniform vec3 cameraShift; // turned, then shifted
        uniform float eye;
        uniform vec4 transform;   // panel to clip space: xy scale, zw offset
        varying vec3 where;       // this point in the scene

        void main()
        {
            where = origin + plane * vec3 (position, 0.0);
            vec3 q = camera * where + cameraShift;
            float w = eye - q.z;
            gl_Position = vec4 (q.xy * eye * transform.xy + transform.zw * w, 0.0, w);
        }
    )";

    // What both field shaders share: the field of one source at a point.
    //
    // As dots, a source fills space with standing waves along its three
    // axes: a tone gives cos (x) * cos (y) * cos (z) times how far the tone
    // is in its swing, a grid of dots with opposite neighbours. A product
    // of cosines is a sum of four waves running along the diagonals, and
    // the sum over all the tones of one such wave is the sound itself,
    // mirrored about the frame's moment. That is what a row of the waves
    // texture holds, out to "reach": so four look-ups give the field of
    // the whole sound. As rings it is simpler: the row holds the sound
    // going back in time, and a point shows what left the source as long
    // ago as it is far.
    //
    // The sound only reaches so far. Past that it is read back and forth
    // again, so the field goes on for ever, repeating like a reflection.
    //
    // Source k has rows 8k to 8k + 7: its sound in eight bands of pitch,
    // lowest first, each filling -1..1 with its real strength beside it
    // (bandLevel). With the EQ a point gets only the bands about the pitch
    // the curve gives for its distance from the source, so each distance
    // has its own range of pitch.
    //
    // The colour comes from the scene's colour map (ColourScale.h): pitch
    // one way, amplitude the other. A band at +1 has its colour of the
    // scale, at -1 the opposite hue, and in between the two blended, so it
    // changes with the phase of the sound. The bands at a point are
    // averaged by how strong each is there.
    constexpr const char* fieldCommon = R"(
        uniform sampler2D waves;
        uniform float reach;
        uniform float rings;          // 1: rings, 0: dots
        uniform float eq;             // 1: the EQ is on
        uniform float eqWidth;        // the width of the range of pitch let through, in bands
        uniform float curve[16];      // the pitch at each distance, 0..1 over the eight bands
        uniform vec3 bandColour[8];   // each band's colour on the scale
        uniform float bandLevel[64];  // how strong each band of each source is

        float wave (float along, float row)
        {
            float t = 1.0 - abs (1.0 - mod (abs (along) / reach, 2.0));
            return texture2D (waves, vec2 ((t * 2047.0 + 0.5) / 2048.0, (row + 0.5) / 64.0)).r;
        }

        float shape (vec3 q, float row)
        {
            if (rings > 0.5)
                return wave (length (q), row);

            return 0.25 * (wave (q.x + q.y + q.z, row) + wave (q.x + q.y - q.z, row)
                           + wave (q.x - q.y + q.z, row) + wave (q.y + q.z - q.x, row));
        }

        vec3 tint (vec3 c, float x)
        {
            vec3 opposite = vec3 (max (c.r, max (c.g, c.b)) + min (c.r, min (c.g, c.b))) - c;
            return mix (opposite, c, 0.5 + 0.5 * clamp (x, -1.0, 1.0));
        }

        // The field of source k at a point, given in the source's own axes:
        // its amplitude, and its colour there.
        float field (vec3 q, int k, out vec3 colour)
        {
            float pitch = 0.0;
            if (eq > 0.5)
            {
                float along = clamp (length (q) / 2.0, 0.0, 1.0) * 15.0;
                int i = int (min (floor (along), 14.0));
                pitch = mix (curve[i], curve[i + 1], along - float (i)) * 8.0;
            }

            float a = 0.0, weight = 0.0;
            vec3 sum = vec3 (0.0);
            for (int b = 0; b < 8; ++b)
            {
                float lets = bandLevel[k * 8 + b];
                if (eq > 0.5)
                    lets *= clamp (1.0 + 0.5 * eqWidth - abs (float (b) + 0.5 - pitch), 0.0, 1.0);

                // A band too faint to see is not worth its look-ups.
                if (lets > 0.004)
                {
                    float x = shape (q, float (k * 8 + b));
                    a += lets * x;
                    sum += lets * abs (x) * tint (bandColour[b], x);
                    weight += lets * abs (x);
                }
            }
            colour = sum / max (weight, 1.0e-6);
            return a;
        }
    )";

    // The field on a plane. The amplitude is how bright it is, it fades
    // with distance from the source, and the sources are added as light.
    const juce::String fieldFragmentShader = juce::String (fieldCommon) + R"(
        uniform int count;
        uniform vec3 sourceAt[8];
        uniform mat3 sourceAxes[8];   // the scene to the source's own axes
        uniform float sourceFade[8];
        uniform float alphaScale;
        varying vec3 where;

        void main()
        {
            vec3 light = vec3 (0.0);
            for (int k = 0; k < 8; ++k)
            {
                if (k >= count)
                    break;

                vec3 q = sourceAxes[k] * (where - sourceAt[k]);
                vec3 colour;
                float a = field (q, k, colour) * max (0.0, 1.0 - sourceFade[k] * length (q));
                light += abs (a) * colour;
            }
            gl_FragColor = vec4 (light, alphaScale);
        }
    )";

    // The Field mode as a volume. Every pixel looks along a line from the
    // eye into the scene; the shader walks that line through a ball that
    // holds the field, and at every step adds the light of the field there
    // (as fieldFragmentShader works it out) and lets it hide what comes
    // after, by the density and how strong the field is there. Each pixel starts a different fraction of a
    // step in, which turns the steps' banding into fine grain.
    constexpr const char* volumeVertexShader = R"(
        attribute vec2 position;
        uniform vec4 transform;   // panel to clip space: xy scale, zw offset
        varying vec2 panel;       // this pixel in the panel
        void main()
        {
            panel = (position - transform.zw) / transform.xy;
            gl_Position = vec4 (position, 0.0, 1.0);
        }
    )";

    const juce::String volumeFragmentShader = juce::String (fieldCommon) + R"(
        uniform int count;
        uniform vec3 sourceAt[8];
        uniform mat3 sourceAxes[8];
        uniform float sourceFade[8];
        uniform mat3 cameraBack;  // the viewer's space to the scene, for directions
        uniform vec3 eyeAt;       // where the eye is in the scene
        uniform float eye;
        uniform float bound;      // radius of the ball that holds the field
        uniform float endless;    // 1: nothing holds it, it goes on for ever
        uniform float density;
        uniform float lens;       // radius of the lens, in scene units; 0 is a pinhole
        uniform float focus;      // how far in front of the eye things are sharp
        varying vec2 panel;

        void main()
        {
            vec3 start = eyeAt;
            vec3 sight = normalize (vec3 (panel, -eye));
            vec3 dir = cameraBack * sight;
            float b = dot (start, dir);
            float h = b * b - dot (start, start) + bound * bound;
            vec3 light = vec3 (0.0);

            // The stretch of the line to walk: through the ball, or, in an
            // endless field, from a little in front of the eye to far off,
            // in steps that grow with the distance.
            float enter = 1.0, leave = 16.0;
            if (endless < 0.5)
            {
                float root = sqrt (max (h, 0.0));
                enter = max (-b - root, 0.0);
                leave = root - b;
            }

            if (endless > 0.5 || (h > 0.0 && leave > enter))
            {
                float jitter = fract (sin (dot (gl_FragCoord.xy, vec2 (12.9898, 78.233))) * 43758.5453);
                float spin = fract (sin (dot (gl_FragCoord.xy, vec2 (39.3468, 11.1351))) * 24634.6345) * 6.2832;
                float through = 1.0;

                for (int i = 0; i < 64; ++i)
                {
                    float f = (float (i) + jitter) / 64.0;
                    float t = endless > 0.5 ? enter * pow (leave / enter, f) : mix (enter, leave, f);
                    float pace = endless > 0.5 ? t * log (leave / enter) / 64.0 : (leave - enter) / 64.0;
                    vec3 p = start + dir * t;

                    // The depth of field. Light from a point reaches the
                    // eye through every part of a lens, and only for points
                    // at the focus distance does all of it land on the same
                    // pixel. So every step looks through another part of
                    // the lens: it is moved sideways by how far that part
                    // is from the lens's middle, times how far off the
                    // focus distance the step is. What is in focus stays
                    // put, the rest is smeared over a disc.
                    if (lens > 0.0)
                    {
                        float turn = float (i) * 2.39996 + spin;
                        float wide = lens * sqrt ((mod (float (i) * 37.0, 64.0) + 0.5) / 64.0);
                        float off = clamp (1.0 + t * sight.z / focus, -3.0, 3.0);
                        p += cameraBack * vec3 (cos (turn), sin (turn), 0.0) * (wide * off);
                    }

                    vec3 glow = vec3 (0.0);
                    float thick = 0.0;

                    for (int k = 0; k < 8; ++k)
                    {
                        if (k >= count)
                            break;

                        vec3 q = sourceAxes[k] * (p - sourceAt[k]);
                        vec3 colour;
                        float a = field (q, k, colour);
                        float dim = max (0.0, 1.0 - sourceFade[k] * length (q));

                        // How solid a point is follows the amplitude, so the
                        // shape of the sound's wave is the shape of a ball
                        // from its edge to its middle: a smooth wave makes
                        // soft clouds, a steep one hard-edged blocks. A
                        // shell of a ring would hide all the shells inside
                        // it, so rings let most of the light through.
                        float m = abs (a) * dim * (rings > 0.5 ? 0.3 : 2.0);
                        glow += m * colour;
                        thick += m;
                    }

                    // In an endless field the far away sinks into the dark.
                    float covers = 1.0 - exp (-thick * density * pace);
                    float haze = endless > 0.5 ? exp (-0.12 * t) : 1.0;
                    light += through * covers * haze * glow / max (thick, 1.0e-5);
                    through *= 1.0 - covers;
                    if (through < 0.02)
                        break;
                }
            }

            // It goes into both the afterglow and the beam, so half each.
            gl_FragColor = vec4 (0.5 * light, 1.0);
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

    using scope::plotRadius;
    constexpr int panelMargin = 16;        // logical px around each square
    constexpr float baseExposure = 1.2f;   // at 100 % brightness
    constexpr float haloGain = 1.5f;
    constexpr int maxBlurRadius = 48;
    constexpr int previewMaxWidth = 640;
    constexpr float barY = -0.95f;         // correlation bar height in the goniometer
    constexpr float fieldSheet = 40.0f;    // half the side of a plane of a field: it goes on for ever, this is far enough

    using scope::leftColour;
    using scope::rightColour;
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
// One layer of a scope: the buffers its picture builds up in. Lives on the
// GL thread.
class ScopeComponent::LayerRenderer
{
public:
    LayerRenderer (ScopeComponent& ownerToUse, const scope::CurveFeed& feedToDraw)
        : owner (ownerToUse), feed (feedToDraw)
    {
    }

    ~LayerRenderer()
    {
        for (auto* target : { &accumTarget, &currentTarget, &blurTempTarget, &blurTarget, &volumeTarget })
            target->release();
        if (fieldTexture != 0)
            glDeleteTextures (1, &fieldTexture);
    }

    // Brings accumTarget, currentTarget and blurTarget up to date.
    void render (View, const scope::Camera& camera, int w, int h, float scale, double dtMs);

    ScopeComponent& owner;
    const scope::CurveFeed& feed;
    bool seen = true;                                // still asked for this frame
    Look look {};                                    // what the last render used
    std::shared_ptr<const scope::Frame> frame;       // and the frame it drew, if there is one yet
    int blurRadius = 0;
    RenderTarget accumTarget, currentTarget, blurTempTarget, blurTarget;

private:
    void uploadField();
    void useSources (juce::OpenGLShaderProgram&);
    void drawField (const PanelPlacement&, const scope::Camera& camera, float alphaScale);
    void drawVolume (const PanelPlacement&, const scope::Camera& camera);

    RenderTarget volumeTarget;                       // the field as a volume, at half resolution

    GLuint fieldTexture = 0;                         // the sources' waves, one row each
    const scope::Frame* fieldUploaded = nullptr;     // the frame whose waves are in it
    std::vector<float> fieldPixels;
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

ScopeComponent::ScopeComponent (const scope::CurveFeed& feed, bool labels)
    : ScopeComponent ([&feed] (LayerList& list) { list[0] = { &feed, {} }; return 1; }, labels)
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
    if (shownLayout.load() < 0)
        return;   // a plane turned in space: the labels have no fixed place

    const auto layout = (Layout) juce::jlimit (0, 3, shownLayout.load());
    if (layout == Layout::mono)
        return;
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
    fieldShader = buildShader (context, fieldVertexShader, fieldFragmentShader.toRawUTF8());
    volumeShader = buildShader (context, volumeVertexShader, volumeFragmentShader.toRawUTF8());
    blurShader = buildShader (context, fullScreenVertexShader, blurFragmentShader);
    blendShader = buildShader (context, fullScreenVertexShader, blendFragmentShader);
    copyShader = buildShader (context, fullScreenVertexShader, copyFragmentShader);
    if (lineShader == nullptr || fieldShader == nullptr || blurShader == nullptr || blendShader == nullptr || copyShader == nullptr)
    {
        lineShader.reset();
        fieldShader.reset();
        volumeShader.reset();
        blurShader.reset();
        blendShader.reset();
        copyShader.reset();
        return;
    }

    linePosition = glGetAttribLocation (lineShader->getProgramID(), "position");
    lineOther = glGetAttribLocation (lineShader->getProgramID(), "other");
    lineDepth = glGetAttribLocation (lineShader->getProgramID(), "depth");
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
    fieldShader.reset();
    volumeShader.reset();
    blurShader.reset();
    blendShader.reset();
    copyShader.reset();
}

//==============================================================================
// How a layer is to be shown, or the defaults where there is none.
ScopeComponent::Look ScopeComponent::readLook (const ScopeParamSet* params)
{
    using namespace scope;
    auto get = [params] (int index) { return params != nullptr ? params->get (index) : paramTable[index].def; };

    return { juce::jlimit (0, 9, (int) get (pBlend)),
             get (pAfterglow),
             get (pBrightness) * 0.01f,
             get (pContrast) * 0.01f,
             get (pBlur),
             get (pViewYaw),
             get (pViewPitch),
             get (pViewRoll),
             { get (pCamX), get (pCamY), get (pCamZ) },
             get (pFieldVolume) > 0.5f,
             0.5f * std::pow (80.0f, get (pFieldDensity) * 0.01f),
             get (pFieldFocus),
             get (pFieldAperture) * 0.01f };
}

scope::Camera ScopeComponent::cameraOf (const Look& look)
{
    return { look.viewYaw, look.viewPitch, look.viewRoll, look.cameraAt };
}

// flat: the plane lies in the panel, so nothing can fall outside its square.
// A plane turned in space can reach past it, and may draw anywhere.
ScopeComponent::PanelPlacement ScopeComponent::placePanel (Layout layout, int panel, int w, int h, int margin, View view, bool flat)
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
             flat ? b.getIntersection ({ 0, 0, w, h }) : juce::Rectangle<int> (0, 0, w, h) };
}

// Half the line thickness (~2.4 px in all), in panel coordinates. It follows
// the canvas, so every tile of a divided picture draws lines equally thick.
float ScopeComponent::lineHalfWidthFor (Layout layout, View view, int w, int h, int margin, float scale)
{
    const int side = panelBounds (layout, 0, juce::roundToInt ((float) w / juce::jmax (1.0e-4f, view.w)),
                                  juce::roundToInt ((float) h / juce::jmax (1.0e-4f, view.h)), margin).getWidth();
    return 1.2f * scale / (float) side;
}

// Aims the line shader at a panel, for points in the scene (orient: the
// camera) or on a plane (the plane as the camera sees it), with lines of
// the given thickness. False if none of it is on this screen.
bool ScopeComponent::usePanel (const PanelPlacement& p, const scope::Mat3& planeToView, float halfWidth, scope::Vec3 shift)
{
    if (p.scissor.isEmpty())
        return false;

    glScissor (p.scissor.getX(), p.scissor.getY(), p.scissor.getWidth(), p.scissor.getHeight());
    setLineTransform (p.scaleX, p.scaleY, p.offsetX, p.offsetY);
    lineShader->setUniformMat3 ("orient", planeToView.m, 1, GL_FALSE);
    lineShader->setUniform ("shift", shift.x, shift.y, shift.z);
    lineShader->setUniform ("halfWidth", halfWidth);
    return true;
}

//==============================================================================
void ScopeComponent::addFullScreenQuad (float r, float g, float b, float a)
{
    auto v = [=] (float x, float y) { return Vertex { x, y, r, g, b, a }; };
    vertices.insert (vertices.end(), { v (-1, -1), v (1, -1), v (1, 1), v (-1, -1), v (1, 1), v (-1, 1) });
}

// The guides are half as thick as the traces.
void ScopeComponent::addTrajectoryGuides()
{
    using scope::addSegment;
    const float c = 0.18f, thin = 0.5f;
    auto v = [c] (float x, float y) { return Vertex { x, y, c, c, c * 1.1f, 1.0f }; };

    addSegment (vertices, v (-1.0f, 0.0f), v (1.0f, 0.0f), thin);
    addSegment (vertices, v (0.0f, -1.0f), v (0.0f, 1.0f), thin);

    constexpr int steps = 128;
    for (int i = 0; i < steps; ++i)
    {
        const float a0 = juce::MathConstants<float>::twoPi * (float) i / steps;
        const float a1 = juce::MathConstants<float>::twoPi * (float) (i + 1) / steps;
        addSegment (vertices, v (plotRadius * std::cos (a0), plotRadius * std::sin (a0)),
                    v (plotRadius * std::cos (a1), plotRadius * std::sin (a1)), thin);
    }
}

void ScopeComponent::addGoniometerGuides (float shown)
{
    using scope::addSegment;
    const float c = 0.18f, thin = 0.5f;
    auto v = [c] (float x, float y, float k = 1.0f) { return Vertex { x, y, c * k, c * k, c * 1.1f * k, 1.0f }; };

    const float d = plotRadius * 0.7071f;
    addSegment (vertices, v (0.0f, -plotRadius), v (0.0f, plotRadius), thin);           // mono (M)
    addSegment (vertices, v (-plotRadius, 0.0f), v (plotRadius, 0.0f), thin);           // anti-phase (S)
    addSegment (vertices, v (-d, -d, 0.6f), v (d, d, 0.6f), thin);                       // R only
    addSegment (vertices, v (-d, d, 0.6f), v (d, -d, 0.6f), thin);                       // L only

    // Correlation bar: track, centre tick, and a marker coloured by sign.
    const float barHalf = 0.85f;
    addSegment (vertices, v (-barHalf, barY), v (barHalf, barY), thin);
    addSegment (vertices, v (0.0f, barY - 0.02f), v (0.0f, barY + 0.02f), thin);

    const auto markerColour = shown > 0.3f ? juce::Colour (0xff97c459)
                            : shown >= 0.0f ? juce::Colour (0xffef9f27)
                                            : juce::Colour (0xffe24b4a);
    const float mx = barHalf * juce::jlimit (-1.0f, 1.0f, shown);
    const Vertex top { mx, barY + 0.035f, markerColour.getFloatRed(), markerColour.getFloatGreen(), markerColour.getFloatBlue(), 1.0f };
    Vertex bottom = top;
    bottom.y = barY - 0.035f;
    addSegment (vertices, bottom, top, thin * 3.0f);
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

    // Only the line shader has colours, and with them the lines' corners.
    const GLint otherAttrib = colourAttrib >= 0 ? lineOther : -1;
    const GLint depthAttrib = colourAttrib >= 0 ? lineDepth : -1;
    if (otherAttrib >= 0)
    {
        glEnableVertexAttribArray ((GLuint) otherAttrib);
        glVertexAttribPointer ((GLuint) otherAttrib, 3, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                               reinterpret_cast<void*> (6 * sizeof (float)));
    }
    if (depthAttrib >= 0)
    {
        glEnableVertexAttribArray ((GLuint) depthAttrib);
        glVertexAttribPointer ((GLuint) depthAttrib, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                               reinterpret_cast<void*> (9 * sizeof (float)));
    }

    glDrawArrays (GL_TRIANGLES, 0, (GLsizei) toDraw.size());

    glDisableVertexAttribArray ((GLuint) positionAttrib);
    if (colourAttrib >= 0)
        glDisableVertexAttribArray ((GLuint) colourAttrib);
    if (otherAttrib >= 0)
        glDisableVertexAttribArray ((GLuint) otherAttrib);
    if (depthAttrib >= 0)
        glDisableVertexAttribArray ((GLuint) depthAttrib);
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
    lineShader->setUniform ("eye", scope::eyeDistance);
    lineShader->setUniform ("halfWidth", 0.0f);
    lineShader->setUniformMat3 ("orient", scope::Mat3().m, 1, GL_FALSE);
    lineShader->setUniform ("shift", 0.0f, 0.0f, 0.0f);
    setLineTransform (1.0f, 1.0f, 0.0f, 0.0f);
}

void ScopeComponent::setLineTransform (float scaleX, float scaleY, float offsetX, float offsetY)
{
    lineShader->setUniform ("transform", scaleX, scaleY, offsetX, offsetY);
}

//==============================================================================
void ScopeComponent::LayerRenderer::render (View view, const scope::Camera& camera, int w, int h, float scale, double dtMs)
{
    // Take the newest frame, and say this feed is being watched so one that
    // is made on demand gets its next. With no new one the last is drawn
    // again; with none at all yet, only the afterglow is kept up.
    if (auto newest = feed.getLatest())
        frame = std::move (newest);
    feed.request();

    look = readLook (&feed.params);
    const auto layout = frame != nullptr ? frame->layout : Layout::sideBySide;
    const bool flat = (frame == nullptr || frame->flat) && camera.atRest();

    // The afterglow decays by the time since the last frame, so it does not
    // depend on the frame rate.
    const float keep = look.afterglowMs >= 1.0f ? (float) std::exp (-dtMs / (double) look.afterglowMs) : 0.0f;
    const int margin = juce::roundToInt ((float) panelMargin * scale);
    const float halfWidth = lineHalfWidthFor (layout, view, w, h, margin, scale);

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
    if (frame != nullptr && frame->numSources > 0)
    {
        uploadField();
        const auto placement = placePanel (layout, 0, w, h, margin, view, false);

        if (look.fieldVolume && owner.volumeShader != nullptr)
        {
            // The volume once, at half size, then that onto both buffers.
            volumeTarget.ensure (w / 2, h / 2);
            volumeTarget.bind();
            glDisable (GL_BLEND);
            drawVolume (placement, camera);
            glEnable (GL_BLEND);

            glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE);
            owner.copyShader->use();
            owner.copyShader->setUniform ("image", (GLint) 0);
            for (auto* target : { &accumTarget, &currentTarget })
            {
                target->bind();
                glBlendColor (0.0f, 0.0f, 0.0f, target == &accumTarget ? 1.0f - keep : 1.0f);
                bindTexture (0, volumeTarget.texture);
                owner.drawFullScreen (*owner.copyShader);
            }
            bindTexture (0, 0);
            glBlendFunc (GL_SRC_ALPHA, GL_ONE);
        }
        else
        {
            accumTarget.bind();
            drawField (placement, camera, 1.0f - keep);
            currentTarget.bind();
            drawField (placement, camera, 1.0f);
        }
    }

    glEnable (GL_SCISSOR_TEST);
    for (int panel = 0; frame != nullptr && frame->numSources == 0 && panel < numPanels (layout); ++panel)
    {
        const auto placement = placePanel (layout, panel, w, h, margin, view, flat);

        for (auto* target : { &accumTarget, &currentTarget })
        {
            target->bind();
            owner.useLineShader (target == &accumTarget ? 1.0f - keep : 1.0f);
            if (owner.usePanel (placement, camera.turn, halfWidth, camera.shift))
                owner.drawVertices (frame->panels[panel].triangles, owner.linePosition, owner.lineColour);
        }
    }
    glDisable (GL_SCISSOR_TEST);

    // 3. Gaussian blur of (afterglow + beam) at half resolution, two passes.
    blurRadius = juce::jlimit (0, maxBlurRadius, juce::roundToInt (look.blurPx * scale * 0.5f));
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

// Puts the mirrored sound of the frame's sources in the texture, one row each.
void ScopeComponent::LayerRenderer::uploadField()
{
    constexpr int across = scope::fieldSamples, rows = scope::maxFieldSources * scope::fieldBands;

    if (fieldTexture == 0)
    {
        glGenTextures (1, &fieldTexture);
        glBindTexture (GL_TEXTURE_2D, fieldTexture);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, across, rows, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        fieldUploaded = nullptr;
    }

    if (fieldUploaded == frame.get())
        return;
    fieldUploaded = frame.get();

    fieldPixels.assign ((size_t) (across * rows), 0.0f);
    for (int k = 0; k < juce::jmin (frame->numSources, scope::maxFieldSources); ++k)
    {
        // A source's rows: its bands of pitch.
        const auto& samples = frame->sources[k].samples;
        std::copy_n (samples.begin(), juce::jmin (samples.size(), (size_t) (across * scope::fieldBands)),
                     fieldPixels.begin() + k * across * scope::fieldBands);
    }

    glBindTexture (GL_TEXTURE_2D, fieldTexture);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, across, rows, GL_RED, GL_FLOAT, fieldPixels.data());
    glBindTexture (GL_TEXTURE_2D, 0);
}

// Tells a field shader about the frame's sources, and gives it their sound.
void ScopeComponent::LayerRenderer::useSources (juce::OpenGLShaderProgram& shader)
{
    const int count = juce::jmin (frame->numSources, scope::maxFieldSources);
    float at[scope::maxFieldSources * 3] {}, fade[scope::maxFieldSources] {};
    float level[scope::maxFieldSources * scope::fieldBands] {};
    float axes[scope::maxFieldSources * 9] {};

    for (int k = 0; k < count; ++k)
    {
        const auto& s = frame->sources[k];
        at[k * 3] = s.at.x;     at[k * 3 + 1] = s.at.y;     at[k * 3 + 2] = s.at.z;
        fade[k] = s.fade;
        std::copy_n (s.level, scope::fieldBands, level + k * scope::fieldBands);

        // The plane turned back: its transpose.
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                axes[k * 9 + col * 3 + row] = s.plane.m[row * 3 + col];
    }

    shader.use();
    const auto program = shader.getProgramID();
    glUniform3fv (glGetUniformLocation (program, "sourceAt"), scope::maxFieldSources, at);
    glUniform3fv (glGetUniformLocation (program, "bandColour"), scope::fieldBands, &frame->bandColour[0][0]);
    glUniform1fv (glGetUniformLocation (program, "bandLevel"), scope::maxFieldSources * scope::fieldBands, level);
    glUniform1fv (glGetUniformLocation (program, "sourceFade"), scope::maxFieldSources, fade);
    glUniformMatrix3fv (glGetUniformLocation (program, "sourceAxes"), scope::maxFieldSources, GL_FALSE, axes);
    shader.setUniform ("count", (GLint) count);
    shader.setUniform ("reach", juce::jmax (1.0e-3f, frame->reach));
    shader.setUniform ("rings", frame->rings ? 1.0f : 0.0f);
    shader.setUniform ("eq", frame->eq ? 1.0f : 0.0f);
    shader.setUniform ("eqWidth", frame->eqWidth / scope::fieldBandOctaves);
    glUniform1fv (glGetUniformLocation (program, "curve"), scope::fieldCurvePoints, frame->curve);
    shader.setUniform ("eye", scope::eyeDistance);
    shader.setUniform ("waves", (GLint) 0);
    bindTexture (0, fieldTexture);
}

// The field of the frame, on each of the planes its sources lie on,
// into the bound target.
void ScopeComponent::LayerRenderer::drawField (const PanelPlacement& p, const scope::Camera& camera, float alphaScale)
{
    auto& shader = *owner.fieldShader;
    useSources (shader);
    shader.setUniform ("alphaScale", alphaScale);
    shader.setUniform ("transform", p.scaleX, p.scaleY, p.offsetX, p.offsetY);
    shader.setUniformMat3 ("camera", camera.turn.m, 1, GL_FALSE);
    shader.setUniform ("cameraShift", camera.shift.x, camera.shift.y, camera.shift.z);

    owner.vertices.clear();
    auto v = [] (float x, float y) { return Vertex { x * fieldSheet, y * fieldSheet, 0.0f, 0.0f, 0.0f, 0.0f }; };
    owner.vertices.insert (owner.vertices.end(), { v (-1, -1), v (1, -1), v (1, 1), v (-1, -1), v (1, 1), v (-1, 1) });
    const auto position = glGetAttribLocation (shader.getProgramID(), "position");

    // Each plane once, however many sources lie on it.
    const int count = juce::jmin (frame->numSources, scope::maxFieldSources);
    for (int k = 0; k < count; ++k)
    {
        const auto& s = frame->sources[k];
        bool drawn = false;
        for (int j = 0; j < k; ++j)
            drawn = drawn || (frame->sources[j].plane.isNear (s.plane) && frame->sources[j].at.isNear (s.at));
        if (drawn)
            continue;

        shader.setUniformMat3 ("plane", s.plane.m, 1, GL_FALSE);
        shader.setUniform ("origin", s.at.x, s.at.y, s.at.z);
        owner.drawVertices (owner.vertices, position, -1);
    }
    bindTexture (0, 0);
}

// The field of the frame as a volume, over the whole of the bound target.
void ScopeComponent::LayerRenderer::drawVolume (const PanelPlacement& p, const scope::Camera& camera)
{
    auto& shader = *owner.volumeShader;
    useSources (shader);

    // A ball about the centre of the scene that holds every source's field,
    // as far as its fade lets it reach; it has to stay in front of the eye.
    // With no fade, or one that reaches past the eye (the camera may have
    // moved into it), the field is endless.
    float bound = 0.5f;
    bool endless = false;
    for (int k = 0; k < juce::jmin (frame->numSources, scope::maxFieldSources); ++k)
    {
        const auto& s = frame->sources[k];
        endless = endless || s.fade <= 0.01f;
        bound = juce::jmax (bound, std::sqrt (s.at.x * s.at.x + s.at.y * s.at.y + s.at.z * s.at.z) + 1.0f / juce::jmax (0.01f, s.fade));
    }

    // The camera turned back: its transpose.
    scope::Mat3 back;
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            back.m[col * 3 + row] = camera.turn.m[row * 3 + col];

    const auto eyeAt = back * scope::Vec3 { -camera.shift.x, -camera.shift.y, scope::eyeDistance - camera.shift.z };
    const float eyeOut = std::sqrt (eyeAt.x * eyeAt.x + eyeAt.y * eyeAt.y + eyeAt.z * eyeAt.z);
    endless = endless || bound > eyeOut - 0.5f;
    shader.setUniform ("bound", juce::jmax (0.01f, juce::jmin (bound, eyeOut - 0.5f)));
    shader.setUniform ("eyeAt", eyeAt.x, eyeAt.y, eyeAt.z);
    shader.setUniform ("endless", endless ? 1.0f : 0.0f);
    shader.setUniform ("density", look.fieldDensity);
    shader.setUniform ("lens", look.fieldAperture * 0.5f);
    shader.setUniform ("focus", juce::jmax (0.5f, scope::eyeDistance - look.fieldFocus));
    shader.setUniform ("transform", p.scaleX, p.scaleY, p.offsetX, p.offsetY);
    shader.setUniformMat3 ("cameraBack", back.m, 1, GL_FALSE);
    owner.drawFullScreen (shader);
    bindTexture (0, 0);
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
    // afterglow carries on from frame to frame.
    for (auto& layer : layers)
        layer->seen = false;

    LayerRenderer* drawn[maxLayers] {};
    for (int i = 0; i < count; ++i)
    {
        auto found = std::find_if (layers.begin(), layers.end(),
                                   [&] (const auto& layer) { return &layer->feed == list[(size_t) i].feed && ! layer->seen; });
        if (found == layers.end())
        {
            layers.push_back (std::make_unique<LayerRenderer> (*this, *list[(size_t) i].feed));
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

    // One camera for the whole scene, so turning it turns the drawing as a
    // whole: that of the lowest layer that has turned its view, so it can be
    // done from any one instance without knowing which is at the bottom.
    // Moving it counts as turning it.
    scope::Camera camera;
    for (int i = 0; i < count && camera.atRest(); ++i)
        camera = cameraOf (readLook (&list[(size_t) i].feed->params));

    for (int i = 0; i < count; ++i)
        drawn[i]->render (list[(size_t) i].view, camera, w, h, scale, dt);

    // The canvas starts as the background with the guides, which are not
    // affected by any image control. A frame says which of its planes want
    // guides; every plane gets them once, from the lowest layer on it. With
    // nothing to draw yet, the default picture's.
    struct Guides { Layout layout; View view; scope::Mat3 planeToView; float correlation; scope::Vec3 shift; };
    Guides guides[maxLayers * scope::maxGuidePlanes];
    int numGuides = 0;
    bool anyFrame = false;
    for (int i = 0; i < count; ++i)
    {
        const auto* frame = drawn[i]->frame.get();
        if (frame == nullptr)
            continue;
        anyFrame = true;

        for (int k = 0; k < frame->numGuides; ++k)
        {
            const auto planeToView = camera.turn * frame->guides[k];
            const auto shift = camera.toView (frame->guidesAt[k]);
            if (std::none_of (guides, guides + numGuides, [&] (const Guides& g) { return g.planeToView.isNear (planeToView) && g.shift.isNear (shift); }))
                guides[numGuides++] = { frame->layout, list[(size_t) i].view, planeToView, frame->correlation, shift };
        }
    }
    if (! anyFrame)
        guides[numGuides++] = { Layout::sideBySide, View(), camera.turn, 1.0f, camera.shift };

    // The labels only fit a single flat picture.
    const auto* bottom = count > 0 ? drawn[0]->frame.get() : nullptr;
    drawnLayout.store (camera.atRest() && (bottom == nullptr || bottom->flat)
                           ? (int) (bottom != nullptr ? bottom->layout : Layout::sideBySide) : -1);

    canvas[0].ensure (w, h);
    canvas[1].ensure (w, h);
    canvas[0].bind();
    glClearColor (backgroundColour.getFloatRed(), backgroundColour.getFloatGreen(), backgroundColour.getFloatBlue(), 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    glBlendFunc (GL_SRC_ALPHA, GL_ONE);
    useLineShader (1.0f);
    glEnable (GL_SCISSOR_TEST);
    for (int i = 0; i < numGuides; ++i)
    {
        const auto& g = guides[i];
        const int margin = juce::roundToInt ((float) panelMargin * scale);
        const float halfWidth = lineHalfWidthFor (g.layout, g.view, w, h, margin, scale);
        const bool flat = g.planeToView.isIdentity() && g.shift.isZero();

        for (int panel = 0; panel < numPanels (g.layout); ++panel)
        {
            if (! usePanel (placePanel (g.layout, panel, w, h, margin, g.view, flat), g.planeToView, halfWidth, g.shift))
                continue;

            vertices.clear();
            if (g.layout == Layout::midSide)
                addGoniometerGuides (g.correlation);
            else
                addTrajectoryGuides();
            drawVertices (vertices, linePosition, lineColour);
        }
    }
    glDisable (GL_SCISSOR_TEST);

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
        blendShader->setUniform ("exposure", baseExposure * layer.look.brightness);
        blendShader->setUniform ("contrast", layer.look.contrast);
        blendShader->setUniform ("haloGain", layer.blurRadius > 0 ? haloGain : 0.0f);
        blendShader->setUniform ("mode", (GLint) layer.look.blend);
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
