#ifndef VELK_RENDER_FRUSTUM_H
#define VELK_RENDER_FRUSTUM_H

#include <velk/api/math_types.h>

namespace velk::render {

/**
 * @brief A view frustum represented as 6 outward-pointing planes
 *        (left, right, bottom, top, near, far).
 *
 * Each plane is encoded as `(a, b, c, d)` such that a point `p` is
 * inside the half-space when `a*p.x + b*p.y + c*p.z + d >= 0`. A point
 * inside the frustum satisfies all 6 inequalities.
 *
 * Extracted from a view-projection matrix via `extract_frustum`. The
 * matrix is assumed to map world-space points to clip-space coordinates
 * with NDC depth range `[0, 1]` (Vulkan / D3D convention) — the
 * near-plane derivation differs for OpenGL's `[-1, 1]` range.
 */
struct Frustum
{
    /// `planes[i] = (nx, ny, nz, d)` with `(nx,ny,nz)` pointing into
    /// the frustum half-space (positive side = inside).
    vec4 planes[6];
};

/**
 * @brief Extracts the 6 frustum planes from a column-major view-
 *        projection matrix using the Gribb-Hartmann method.
 *
 * Plane normals are not normalised; the AABB test below doesn't need
 * unit-length normals. Callers that want signed distances in world
 * units should normalise themselves.
 */
inline Frustum extract_frustum(const mat4& vp)
{
    // Row-extraction helper: vp(row, col).
    auto row = [&](int r) -> vec4 {
        return { vp(r, 0), vp(r, 1), vp(r, 2), vp(r, 3) };
    };
    const vec4 r0 = row(0);
    const vec4 r1 = row(1);
    const vec4 r2 = row(2);
    const vec4 r3 = row(3);

    auto add = [](const vec4& a, const vec4& b) {
        return vec4{ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w };
    };
    auto sub = [](const vec4& a, const vec4& b) {
        return vec4{ a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w };
    };

    Frustum f;
    f.planes[0] = add(r3, r0); // left   (x >= -w)
    f.planes[1] = sub(r3, r0); // right  (x <=  w)
    f.planes[2] = add(r3, r1); // bottom (y >= -w)
    f.planes[3] = sub(r3, r1); // top    (y <=  w)
    f.planes[4] = r2;          // far    (z >=  0)  Vulkan / D3D depth range, reversed
    f.planes[5] = sub(r3, r2); // near   (z <=  w)
    return f;
}

/**
 * @brief Returns false if @p box is entirely on the negative side of
 *        any frustum plane (i.e., fully outside the frustum).
 *
 * Conservative: a box partially outside but not fully on one side of
 * any single plane returns true. That under-culls slightly when a
 * corner happens to clip several planes simultaneously, but never
 * over-culls (no false negatives).
 */
inline bool aabb_in_frustum(const Frustum& f, const aabb& box)
{
    const vec3 lo = box.min();
    const vec3 hi = box.max();
    for (int i = 0; i < 6; ++i) {
        const vec4 p = f.planes[i];
        // The AABB corner farthest along the plane normal: pick max for
        // each axis where the normal is positive, min where negative.
        // If even that corner is on the negative side, the whole box is
        // outside this plane.
        const float x = (p.x >= 0.f) ? hi.x : lo.x;
        const float y = (p.y >= 0.f) ? hi.y : lo.y;
        const float z = (p.z >= 0.f) ? hi.z : lo.z;
        if (p.x * x + p.y * y + p.z * z + p.w < 0.f) {
            return false;
        }
    }
    return true;
}

} // namespace velk::render

#endif // VELK_RENDER_FRUSTUM_H
