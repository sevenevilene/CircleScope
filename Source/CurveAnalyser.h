#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "ScopeCurve.h"

// Turns audio into the drawing: reads the samples of an instance, does the
// FFT work (analytic signal, band limits), and builds each frame's curve by
// the instance's mode, layout and plane. It runs on a thread of its own and
// publishes the frames to a CurveFeed.
//
// The modes, per channel (Side by side / Overlay / Mono layouts):
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
//   Axis:        the signal moves a point along one axis and nothing else:
//                up and down (y) on the XY plane, in depth (z) on YZ, left
//                and right (x) on XZ.
//   Field:       not a curve but a field of standing waves. Space is filled
//                with dots that stay where they are and swing between a
//                colour and its complement, through dark: every tone in the
//                sound makes a grid of them, neighbours opposite, as far
//                apart as the tone's wavelength (the Window is the scale:
//                one Window of sound to a plane unit). So a low note is
//                large dots, the highest are a pixel each, like fine noise,
//                and a sound is all of its tones' grids added. The grid
//                runs along the three axes of the instance's plane, from
//                its position, so in space the dots are spheres and every
//                plane cuts through them. The scope shows them as a volume
//                looked into from the camera, or only the planes' cuts, in
//                both cases the fields of all the instances summed here.
//                A standing wave is the sound running both ways at once:
//                the analyser hands on the sound mirrored about one moment
//                (scope::FieldSource), and the scope works out every pixel
//                from it on the graphics card. It is the channels' average,
//                whatever the layout. The swing is far too fast to see, so
//                Field lock works like a stroboscope: it takes the moment
//                from the same point of the wave every frame, moved on a
//                little each time (Field speed), and the dots pulse slowly.
//                Field shape can be Rings instead: then each instance is a
//                point the sound spreads out from in all directions, so at
//                a distance d from it you find the sound of d Windows ago,
//                and a tone makes rings (shells, in space). Locked, they
//                move outward slowly.
//                With Field EQ on, the sound is handed on split into bands
//                of pitch, and the scope lets through at every distance
//                from the instance only those about the pitch the
//                instance's curve gives for that distance.
// Mid / side layout: a goniometer. Vertical = mid (L+R), horizontal = side
// (R-L). Mono layout: one trajectory, of the two channels' average; a mono
// source is always drawn this way.
//
// Low cut / High cut band-limit the signal in the frequency domain before
// any of that.
//
// Each instance's picture lies on its plane in the 3D scene (Plane.h). The
// analyser can add other instances to its own (its partners): at every
// moment each gives a point on its plane, and the sum of those is one point
// in space, so together they draw a single curve in 3D. The instance's own
// settings say what the whole picture is (layout, window, colours); every
// partner keeps its mode, delay, band limits, plane and auto-gain. Where
// the instances say where their samples are on a shared clock
// (ScopeSource::clockOffset), the sum is of samples from the same moment;
// otherwise of each one's newest.
//
// It works when asked: a frame each time a scope requests one from the
// feed, and about sixty a second while the feed is being streamed.
class CurveAnalyser : private juce::Thread
{
public:
    static constexpr int maxSignals = 8;         // the instance and up to seven partners

    // The instances to add to the analyser's own, asked for every frame.
    using Partners = std::function<std::vector<std::shared_ptr<const ScopeSource>>()>;

    // own must outlive the analyser; partners are kept alive while in use.
    CurveAnalyser (const ScopeSource& own, scope::CurveFeed& feedToFill, Partners partnersToAdd = {});
    ~CurveAnalyser() override;

private:
    using Mode = scope::Mode;
    using Layout = scope::Layout;

    struct Settings
    {
        Mode mode;
        bool fixedColour, guides;
        Layout layout;
        bool swingRound;
        float swingHz;
        float windowMs, delayMs;
        float lowCutHz, highCutHz;
        double sampleRate;
        scope::Mat3 plane;         // plane to scene
        bool axisAlongU;           // Axis mode: the point moves along the plane's u, not its v
        scope::Vec3 at;            // the plane's centre in the scene
        float fieldHue, fieldFade; // Field mode: degrees, and amplitude lost per plane unit
        bool fieldLock;
        float fieldSpeed;          // of the locked picture, as a fraction of real time
        bool fieldRings;
        bool fieldEq;
        float fieldEqWidth;        // octaves
    };

    // Sample range of the analysis block that gets drawn this frame.
    struct Range { int start, end, length, stride, freqLag; };

    static constexpr int fftOrder = 14;
    static constexpr int blockSize = 1 << fftOrder;   // 16384 samples
    static constexpr int edgeMargin = 1024;          // skip FFT edge artefacts

    // One instance's audio, analysed.
    struct Signal
    {
        Signal (const ScopeSource& s, std::shared_ptr<const ScopeSource> keepAlive) : source (s), keep (std::move (keepAlive))
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                block[ch].resize ((size_t) blockSize);
                analytic[ch].resize ((size_t) blockSize);
            }
        }

        const ScopeSource& source;
        std::shared_ptr<const ScopeSource> keep;
        Settings settings {};
        std::vector<float> block[2];
        std::vector<std::complex<float>> analytic[2];    // z(t) = s(t) + i*H{s}(t)
        std::vector<float> bandWeights;                  // per FFT bin, 0..N/2
        float weightsLowCut = -1.0f, weightsHighCut = -1.0f;
        double weightsSampleRate = 0.0;
        bool bandLimited = false;
        float gain[4] { 1.0f, 1.0f, 1.0f, 1.0f };        // per Curve
        float fieldGain = 1.0f;                          // of its wave in the Field mode
        size_t blockStart[2] {};                         // absolute position of each block's first sample
    };

    struct Point3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    enum Curve { leftCurve = 0, rightCurve = 1, goniometerCurve = 2, monoCurve = 3 };

    void run() override;
    void makeFrame();
    void updateSignals();
    void readSamples();
    Range computeRange (const Settings&) const;
    void updateBandWeights (Signal&);
    void analyse (Signal&, int channel);
    void updateCorrelation (const Range&);
    float updateGain (Signal&, Curve, const Range&);
    void addField (scope::Frame&, const Range&);
    void addCurve (scope::Frame::Panel&, Curve, const Range&, std::optional<juce::Colour> colourOverride);

    static Settings readSettings (const ScopeSource&);
    static juce::Colour colourForFrequency (double hz);

    const ScopeSource& source;
    scope::CurveFeed& feed;
    const Partners partners;

    juce::dsp::FFT fft { fftOrder };
    scope::FramePool pool;
    std::vector<std::unique_ptr<Signal>> signals;    // [0] is the instance's own
    std::vector<std::complex<float>> timeScratch, freqScratch;
    std::vector<std::complex<float>> bandFreq, bandTime;   // Field EQ: one band of a source's sound
    std::vector<float> wave;                         // Field mode: the sound a source's samples are taken from
    std::vector<Point3> points;                      // scratch: the curve being summed
    float sumGain[4] { 1.0f, 1.0f, 1.0f, 1.0f };     // of a sum of several planes, per Curve
    float correlation = 1.0f;
    double lastFrameMs = 0.0;
    double fieldCreep = 0.0, lastFieldMs = 0.0;      // Field mode: how far the locked picture has moved on, in samples

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveAnalyser)
};
