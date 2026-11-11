#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <complex>
#include "PluginProcessor.h"

// Draws each channel as a 2D trajectory in its own square viewport.
//
//   Quadrature:  x = s(t), y = H{s}(t) (90 degrees shifted, via the analytic
//                signal). A pure sine traces a perfect circle.
//   Delay embed: x = s(t), y = s(t - delay). With delay = a quarter period
//                (12.5 ms for 20 Hz) a sine is also a circle.
//
// Extra dimensions: hue = instantaneous frequency (from the analytic
// signal's phase rate), brightness = age of the trail.
class ScopeComponent : public juce::Component, private juce::OpenGLRenderer
{
public:
    explicit ScopeComponent (CircleScopeProcessor&);
    ~ScopeComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Vertex { float x, y, r, g, b, a; };

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void buildChannel (int channel, float halfWidth);
    void addSegment (Vertex a, Vertex b, float halfWidth);
    void addGuides (float halfWidth);

    CircleScopeProcessor& processor;
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    GLuint vbo = 0, vao = 0;
    GLint positionAttrib = -1, colourAttrib = -1;

    std::atomic<int> width { 0 }, height { 0 };

    // Analysis buffers (render thread only).
    static constexpr int fftOrder = 14;
    static constexpr int blockSize = 1 << fftOrder;   // 16384 samples
    static constexpr int edgeMargin = 1024;          // skip FFT edge artefacts
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> block;
    std::vector<std::complex<float>> spectrum, analytic;
    std::vector<Vertex> vertices;
    float gain[2] { 1.0f, 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeComponent)
};
