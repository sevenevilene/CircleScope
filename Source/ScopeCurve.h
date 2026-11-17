#pragma once

#include <juce_graphics/juce_graphics.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Plane.h"
#include "ScopeSource.h"

// The finished drawing of one instance: what a CurveAnalyser makes of the
// audio, what the plugin sends over the network, and all a scope needs to
// draw. Whoever draws it does no analysis and needs no audio, so the
// plugin's window, a wall and every cluster node show the very same points.
namespace scope
{
    constexpr float plotRadius = 0.9f;        // where a full-scale pure tone lands
    constexpr int maxCurvePoints = 4096;      // in one curve
    constexpr int maxPanelPoints = 2 * maxCurvePoints;   // a panel holds one curve, or two (Overlay)
    constexpr int maxGuidePlanes = 8;
    constexpr int maxFieldSources = 8;        // the instance and up to seven summed with it
    constexpr int fieldSamples = 2048;        // of each source's wave, from now back in time
    constexpr float fieldMaxReach = 4.0f;     // how far along its axes a field is ever drawn, in plane units

    inline const juce::Colour leftColour (0xff1d9e75), rightColour (0xffd85a30), goniometerColour (0xff7f77dd);

    // A point of a curve, in the scene's coordinates about its panel's
    // centre, with its colour; alpha is the trail's age.
    struct CurvePoint { float x, y, z, r, g, b, a; };

    // What the line shader takes. A point in the scene, or on a plane
    // (z = 0). The members after the colour are for the corners of a line
    // and filled in by addSegment: the segment's other end, and which side
    // of the line the corner is on times how thick this line is next to the
    // others (the shader has the thickness itself, so it can follow the
    // size of the screen).
    struct Vertex { float x, y, r, g, b, a; float otherX = 0.0f, otherY = 0.0f, width = 0.0f, z = 0.0f, otherZ = 0.0f; };

    // The two triangles of one line segment.
    inline void addSegment (std::vector<Vertex>& out, const Vertex& a, const Vertex& b, float width = 1.0f)
    {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        if (dx * dx + dy * dy + dz * dz < 1.0e-12f)
            return;

        // Four corners, two at each end. The shader moves each one sideways
        // on screen; seen from b the line runs the other way, hence the signs.
        auto corner = [] (Vertex v, const Vertex& otherEnd, float signedWidth)
        {
            v.otherX = otherEnd.x;
            v.otherY = otherEnd.y;
            v.otherZ = otherEnd.z;
            v.width = signedWidth;
            return v;
        };

        const Vertex a0 = corner (a, b, width), a1 = corner (a, b, -width);
        const Vertex b0 = corner (b, a, -width), b1 = corner (b, a, width);
        out.insert (out.end(), { a0, a1, b0, b0, a1, b1 });
    }

    // One source of a field (the Field mode): where it is, the plane whose
    // axes its dots line up with, its colour, and its sound about one
    // moment, the last sample as far from it as the frame's reach at one
    // Window to a plane unit. For dots (standing waves) the sound is
    // mirrored: samples[j] is the average of the sound j steps after the
    // moment and j steps before it. For rings it is the sound itself,
    // going back: samples[j] is the sound j steps before the moment.
    // With the frame's EQ on there are fieldBands such runs, one after the
    // other: the sound split into bands of pitch, lowest first.
    struct FieldSource
    {
        Mat3 plane;
        Vec3 at;
        float r = 1.0f, g = 0.5f, b = 0.0f;     // of +1; -1 gets the complement
        float fade = 0.0f;                       // amplitude lost per plane unit of distance from it
        std::vector<float> samples;              // fieldSamples of them, -1..1; times fieldBands with the EQ on
    };

    // One frame of the drawing: curves, or a wave field.
    struct Frame
    {
        struct Panel
        {
            std::vector<CurvePoint> points;
            int secondStart = -1;                // where the panel's second curve begins, if it has two
            std::vector<Vertex> triangles;       // the points as line segments, see buildTriangles
        };

        Layout layout = Layout::sideBySide;
        bool flat = true;                        // every plane in it is plain XY
        float correlation = 1.0f;                // of left and right, for the goniometer's bar
        int numGuides = 0;                       // the planes in it that want their guides shown
        Mat3 guides[maxGuidePlanes];
        Vec3 guidesAt[maxGuidePlanes];           // each one's centre
        Panel panels[2];

        // A wave field instead of curves, when there are sources.
        int numSources = 0;
        bool rings = false;                      // the sound spreads from the sources as rings; else it stands, as dots
        FieldSource sources[maxFieldSources];
        float reach = 1.0f;                      // the distance the samples cover, in plane units
        bool eq = false;                         // only a range of pitch at each distance from a source:
        float eqWidth = 2.0f;                    // this many octaves wide,
        float curve[fieldCurvePoints] {};        // about the pitch the curve gives for the distance

        void clear()
        {
            layout = Layout::sideBySide;
            flat = true;
            correlation = 1.0f;
            numGuides = 0;
            numSources = 0;
            rings = false;
            eq = false;
            reach = 1.0f;
            for (auto& panel : panels)
            {
                panel.points.clear();
                panel.secondStart = -1;
                panel.triangles.clear();
            }
        }

        // Call once the points are in, before the frame is published.
        void buildTriangles()
        {
            for (auto& panel : panels)
            {
                panel.triangles.clear();
                for (size_t i = 1; i < panel.points.size(); ++i)
                {
                    if ((int) i == panel.secondStart)
                        continue;
                    const auto& p = panel.points[i - 1];
                    const auto& q = panel.points[i];
                    Vertex a { p.x, p.y, p.r, p.g, p.b, p.a }, b { q.x, q.y, q.r, q.g, q.b, q.a };
                    a.z = p.z;
                    b.z = q.z;
                    addSegment (panel.triangles, a, b);
                }
            }
        }
    };

    // Frames to fill in, for one producer. A frame comes round again once
    // nobody holds it any more, so steady running allocates nothing.
    class FramePool
    {
    public:
        std::shared_ptr<Frame> acquire()
        {
            for (auto& frame : frames)
                if (frame.use_count() == 1)
                {
                    frame->clear();
                    return frame;
                }

            frames.push_back (std::make_shared<Frame>());
            return frames.back();
        }

    private:
        std::vector<std::shared_ptr<Frame>> frames;
    };

    // One layer for a scope to draw: the newest frame of a drawing, and the
    // parameters that say how it should look (afterglow, brightness, blend,
    // the camera...). The producer publishes frames; any number of scopes,
    // and the network sender, take the newest.
    class CurveFeed
    {
    public:
        explicit CurveFeed (const ScopeParamSet& lookParams) : params (lookParams) {}

        const ScopeParamSet& params;

        void publish (std::shared_ptr<const Frame> frame)
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            latest = std::move (frame);
            ++serial;
        }

        // The newest frame (null before the first), and a number that
        // changes with every one published.
        std::shared_ptr<const Frame> getLatest (juce::uint32* serialNumber = nullptr) const
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            if (serialNumber != nullptr)
                *serialNumber = serial;
            return latest;
        }

        // A scope says it is drawing this feed, so a producer that only
        // works on demand makes the next frame.
        void request() const { wanted.signal(); }
        bool waitForRequest (int milliseconds) const { return wanted.wait (milliseconds); }

        // Set while the feed is being sent to the network: frames are
        // wanted then even with no scope looking.
        std::atomic<bool> streamed { false };

    private:
        mutable juce::SpinLock mutex;
        std::shared_ptr<const Frame> latest;
        juce::uint32 serial = 0;
        juce::WaitableEvent wanted;
    };
}
