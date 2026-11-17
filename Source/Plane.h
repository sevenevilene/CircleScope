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
    };

    // How far the eye is from the centre of a panel, in plane units. Sets
    // how strong the perspective is.
    constexpr float eyeDistance = 4.0f;
}
