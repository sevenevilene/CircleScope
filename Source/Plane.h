#pragma once

#include <cmath>
#include "ScopeParams.h"

// The space a scope draws in.
//
// Every layer draws on a plane: a flat sheet through the centre of its
// panel, on which the drawing mode lays out its picture in plane coordinates
// (u to the right, v up, -1..1). The plane has an orientation in a 3D scene
// with x to the right, y up and z towards the viewer:
//
//   XY  faces the viewer: u = x, v = y. This is the flat picture.
//   YZ  the side wall:    u = z, v = y.
//   XZ  the floor:        u = x, v = -z (upright when seen from above).
//
// and can be turned further about the scene's x, y and z axes. A plane's
// centre is at the centre of the scene unless it is moved (Position X / Y /
// Z), so three layers on XY, YZ and XZ cross there like the three
// coordinate planes.
//
// One camera looks at the whole scene: yaw turns the scene about the
// vertical axis, pitch tips it to look from above (or below), roll turns
// the picture about the line of sight. Since every layer is seen through
// it, it is what turns a drawing as a whole, a sum of planes included. With the
// camera at rest an XY plane is exactly the flat picture, whatever the
// perspective, because it lies at depth 0.
//
// The camera looks at a point of the scene, the centre unless it is moved
// (Camera X / Y / Z), from eyeDistance away, and its turning is about that
// point. Moving the point moves the camera with it: that is how it travels
// through the scene.
//
// A drawing mode only produces geometry in plane coordinates; the line
// shader takes it through the plane, the camera and the perspective, and
// gives the lines their thickness on screen. So a new mode works on every
// plane, and a new kind of plane works with every mode.
namespace scope
{
    // A point or a direction in the scene.
    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        bool isNear (const Vec3& o) const noexcept
        {
            return std::abs (x - o.x) + std::abs (y - o.y) + std::abs (z - o.z) < 1.0e-4f;
        }

        bool isZero() const noexcept { return isNear ({}); }
    };

    // Rotation, column-major as OpenGL takes it.
    struct Mat3
    {
        float m[9] { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f };

        Mat3 operator* (const Mat3& o) const noexcept
        {
            Mat3 r;
            for (int c = 0; c < 3; ++c)
                for (int row = 0; row < 3; ++row)
                    r.m[c * 3 + row] = m[row] * o.m[c * 3] + m[3 + row] * o.m[c * 3 + 1] + m[6 + row] * o.m[c * 3 + 2];
            return r;
        }

        Vec3 operator* (const Vec3& v) const noexcept
        {
            return { m[0] * v.x + m[3] * v.y + m[6] * v.z,
                     m[1] * v.x + m[4] * v.y + m[7] * v.z,
                     m[2] * v.x + m[5] * v.y + m[8] * v.z };
        }

        bool isNear (const Mat3& o) const noexcept
        {
            for (int i = 0; i < 9; ++i)
                if (std::abs (m[i] - o.m[i]) > 1.0e-4f)
                    return false;
            return true;
        }

        bool isIdentity() const noexcept { return isNear ({}); }

        static Mat3 aboutX (float degrees) noexcept
        {
            const float c = cosDeg (degrees), s = sinDeg (degrees);
            return { { 1.0f, 0.0f, 0.0f,  0.0f, c, s,  0.0f, -s, c } };
        }

        static Mat3 aboutY (float degrees) noexcept
        {
            const float c = cosDeg (degrees), s = sinDeg (degrees);
            return { { c, 0.0f, -s,  0.0f, 1.0f, 0.0f,  s, 0.0f, c } };
        }

        static Mat3 aboutZ (float degrees) noexcept
        {
            const float c = cosDeg (degrees), s = sinDeg (degrees);
            return { { c, s, 0.0f,  -s, c, 0.0f,  0.0f, 0.0f, 1.0f } };
        }

    private:
        static float cosDeg (float d) noexcept { return std::cos (d * 0.017453292f); }
        static float sinDeg (float d) noexcept { return std::sin (d * 0.017453292f); }
    };

    // Plane coordinates to scene coordinates: the base plane, then turned
    // about the scene's x, y and z axes in that order.
    inline Mat3 planeOrientation (Plane base, float rotX, float rotY, float rotZ) noexcept
    {
        const Mat3 preset = base == Plane::yz ? Mat3::aboutY (-90.0f)
                          : base == Plane::xz ? Mat3::aboutX (-90.0f)
                                              : Mat3();
        return Mat3::aboutZ (rotZ) * Mat3::aboutY (rotY) * Mat3::aboutX (rotX) * preset;
    }

    // Scene coordinates to the viewer's.
    inline Mat3 cameraOrientation (float yaw, float pitch, float roll) noexcept
    {
        return Mat3::aboutZ (roll) * Mat3::aboutX (pitch) * Mat3::aboutY (yaw);
    }

    // The whole camera: a point of the scene to the viewer's space is
    // turn * point + shift, and the eye is at (0, 0, eyeDistance) there,
    // looking down the z axis.
    struct Camera
    {
        Mat3 turn;
        Vec3 shift;

        Camera() = default;

        // Looking at "target" in the scene.
        Camera (float yaw, float pitch, float roll, Vec3 target) noexcept
            : turn (cameraOrientation (yaw, pitch, roll))
        {
            const Vec3 t = turn * target;
            shift = { -t.x, -t.y, -t.z };
        }

        bool atRest() const noexcept { return turn.isIdentity() && shift.isZero(); }

        Vec3 toView (Vec3 p) const noexcept
        {
            const Vec3 q = turn * p;
            return { q.x + shift.x, q.y + shift.y, q.z + shift.z };
        }

        // The same camera with what it sees turned once more, about the
        // point it looks at.
        Camera turnedBy (const Mat3& more) const noexcept
        {
            Camera c;
            c.turn = more * turn;
            c.shift = more * shift;
            return c;
        }
    };

    // The side a receiver looks at the scene from. It is the receiver's own
    // (a cell of a wall has one), on top of the scene's camera: the camera
    // turns and moves the scene, and this says which side of the result is
    // shown. So cells showing one stream from several sides work like
    // cameras fixed to each other. Front is the scene as its camera has it.
    enum class Perspective { front = 0, back, top, bottom, left, right };
    constexpr int numPerspectives = 6;
    inline constexpr const char* perspectiveNames[numPerspectives] = { "Front", "Back", "Top", "Bottom", "Left", "Right" };

    // The camera angles that look at the scene from a side. The plugin's
    // camera pad sets them (View yaw / pitch), and a receiver's own side is
    // the same turn.
    struct ViewAngles { float yaw = 0.0f, pitch = 0.0f; };

    inline ViewAngles perspectiveAngles (Perspective from) noexcept
    {
        switch (from)
        {
            case Perspective::back:   return { 180.0f, 0.0f };
            case Perspective::top:    return { 0.0f, 90.0f };      // -z is up, as on the XZ plane
            case Perspective::bottom: return { 0.0f, -90.0f };
            case Perspective::left:   return { 90.0f, 0.0f };
            case Perspective::right:  return { -90.0f, 0.0f };
            case Perspective::front:  break;
        }
        return {};
    }

    inline Mat3 perspectiveTurn (Perspective from) noexcept
    {
        const auto angles = perspectiveAngles (from);
        return cameraOrientation (angles.yaw, angles.pitch, 0.0f);
    }

    // How far the eye is from the centre of a panel, in plane units. Sets
    // how strong the perspective is.
    constexpr float eyeDistance = 4.0f;

    // Stereo: the scene drawn for one eye, or for both. An eye stands to
    // the side of the camera's eye and looks through the same window, the
    // panel at depth 0. So what lies at depth 0 (the flat picture) is the
    // same for both eyes and sits on the screen; what is nearer comes out
    // of it and what is further lies behind it. Like the side, it is the
    // receiver's own: it belongs to the screen, not to the scene.
    //
    //   anaglyph           both in one picture, for red and cyan glasses: the
    //                      left eye's in red, the right eye's in cyan, each
    //                      by its brightness, since the colours go to the
    //                      glasses. What is on the screen plane stays white
    //                      (the two fall on each other); the rest splits
    //                      into a red and a cyan picture, further apart the
    //                      further it is from the plane
    //   wiggle             one picture that goes back and forth between the
    //                      two eyes' some times a second. The jump reads as
    //                      depth with no glasses, on any screen, and the
    //                      colours stay; what is on the screen plane keeps
    //                      still. Each eye has its own afterglow, so they
    //                      do not smear into each other. It goes by the
    //                      clock, so screens whose clocks agree jump together
    //   leftEye, rightEye  one eye's picture: for a pair of projectors, or
    //                      two cells of a wall
    //   sideBySide         both, left eye on the left, each half a picture
    //                      of its own shape: for a viewer, or two projectors
    //                      on one wide output
    //   crossed            the same with the eyes changed over, to look at
    //                      with crossed eyes on any screen
    //   squeezed           both in one picture's shape, each squeezed to
    //                      half its width: what a 3D TV or projector takes
    enum class Stereo { off = 0, anaglyph, wiggle, leftEye, rightEye, sideBySide, crossed, squeezed };
    constexpr int numStereoModes = 8;
    inline constexpr const char* stereoNames[numStereoModes] = { "Mono", "Red / cyan", "Wiggle", "Left eye", "Right eye", "Side by side", "Cross-eyed", "3D TV" };

    // How far apart the eyes are at a depth of 100%, in plane units: a
    // thirtieth of the distance to the screen, as a viewer's are.
    constexpr float eyeSeparation = eyeDistance / 30.0f;
    constexpr int maxStereoDepth = 300;                  // percent
    inline constexpr int stereoDepths[] = { 25, 50, 75, 100, 150, 200, 300 };   // the ones offered

    // How often Wiggle changes eye, a second.
    constexpr int wiggleRate = 8, maxWiggleRate = 30;
    inline constexpr int wiggleRates[] = { 3, 4, 6, 8, 10, 12, 16 };
}
