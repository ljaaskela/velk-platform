struct Ray {
    vec3 origin;
    vec3 dir;
};

struct RayHit {
    float t;
    vec2 uv;
    vec3 normal;
    uint object;      // the object id of what was hit, only set by trace_closest_hit
    // Where on its object the hit is, and which of its axes the surface faces, as the
    // G-buffer's surface attachment has them. Only set by trace_closest_hit.
    vec3 local;
    uint side;
    // What was hit, as the visibility buffer names a pixel, for velk_hit_context: its draw,
    // its instance's index, its cluster (VELK_NO_CLUSTER for none), the vertex indices of its
    // triangle's corners, and the weights of the second and third. corners.x is
    // VELK_NO_CORNERS where an analytic shape was hit, whose uv and normal are the surface's.
    // Only set by trace_closest_hit.
    uint draw;
    uint instance;
    uint cluster;
    uvec3 corners;
    vec2 weights;
    // The kept cut record that was hit (VELK_NO_RECORD for none), as velk_ids_record names a
    // pixel's, and the triangle's index in its cluster's index order, as the record's light
    // counts its triangles (VELK_NO_CORNERS where not known). Only set by trace_closest_hit, and
    // not kept by velk_hit_words.
    uint record;
    uint triangle;
    // A stand-in's flatness (velk_stand_in_flatness) where it is one, a view's quad or a quad of
    // a level of detail standing for small pieces; below 0 for a surface as drawn. Only set by
    // trace_closest_hit.
    float stand_in;
    // The normal of the plane of what was hit, as drawn (its triangle's, whatever its winding),
    // in world space, for which side a ray meets. Only set by trace_closest_hit.
    vec3 plane;
};

#define VELK_NO_CORNERS 0xFFFFFFFFu

// The normal a hit is shaded with: its own, or on a surface seen from both sides
// (VELK_DRAW_DOUBLE_SIDED) turned to the side of its plane that a ray along `dir` meets, as the
// resolve turns a pixel's.
vec3 velk_hit_shading_normal(RayHit hit, vec3 dir)
{
    if (hit.draw == 0xFFFFFFFFu || (velk_draw_data.data[hit.draw].flags & VELK_DRAW_DOUBLE_SIDED) == 0u) return hit.normal;
    return dot(hit.normal, hit.plane) * dot(-dir, hit.plane) < 0.0 ? -hit.normal : hit.normal;
}

// What a hit was, as 7 words for a pass after the trace (velk_hit_context): its draw, its
// instance, its cluster and its corners, then the corners' weights, or for an analytic shape
// its uv, packed. The draw is first, so that a word for a miss can stand in its place.
void velk_hit_words(RayHit hit, out uvec4 a, out uvec3 b)
{
    a = uvec4(hit.draw, hit.instance, hit.cluster, hit.corners.x);
    b = uvec3(hit.corners.y, hit.corners.z,
              hit.corners.x == VELK_NO_CORNERS ? packHalf2x16(hit.uv) : packUnorm2x16(hit.weights));
}

RayHit velk_hit_from_words(uvec4 a, uvec3 b)
{
    RayHit hit;
    hit.t = 0.0;
    hit.normal = vec3(0.0);
    hit.object = 0u;
    hit.local = vec3(0.0);
    hit.side = 0u;
    hit.draw = a.x;
    hit.instance = a.y;
    hit.cluster = a.z;
    hit.corners = uvec3(a.w, b.x, b.y);
    hit.uv = a.w == VELK_NO_CORNERS ? unpackHalf2x16(b.z) : vec2(0.0);
    hit.weights = a.w == VELK_NO_CORNERS ? vec2(0.0) : unpackUnorm2x16(b.z);
    hit.record = VELK_NO_RECORD;
    hit.triangle = VELK_NO_CORNERS;
    hit.stand_in = -1.0;
    hit.plane = vec3(0.0);
    return hit;
}

// Ray vs. oriented rect parameterised by (origin + s*u_axis + t*v_axis) for
// s,t in [0,1]. |u_axis|, |v_axis| carry the extents.
bool intersect_rect(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 u_axis = shape.u_axis.xyz;
    vec3 v_axis = shape.v_axis.xyz;
    vec3 origin = shape.origin.xyz;
    float radius = shape.params.x;

    vec3 normal = cross(u_axis, v_axis);
    float nlen2 = dot(normal, normal);
    if (nlen2 < 1e-12) return false;
    float inv_nlen = inversesqrt(nlen2);
    vec3 n = normal * inv_nlen;

    float denom = dot(ray.dir, n);
    if (abs(denom) < 1e-6) return false;
    float t = dot(origin - ray.origin, n) / denom;
    if (t <= 0.0) return false;

    vec3 p = ray.origin + t * ray.dir;
    vec3 local = p - origin;
    float u_len2 = dot(u_axis, u_axis);
    float v_len2 = dot(v_axis, v_axis);
    float s = dot(local, u_axis) / u_len2;
    float tt = dot(local, v_axis) / v_len2;
    if (s < 0.0 || s > 1.0 || tt < 0.0 || tt > 1.0) return false;

    // Corner radius test, in the same units as the axes.
    if (radius > 0.0) {
        float u_len = sqrt(u_len2);
        float v_len = sqrt(v_len2);
        vec2 size_w = vec2(u_len, v_len);
        vec2 p_w = vec2(s * u_len, tt * v_len);
        vec2 half_size = size_w * 0.5;
        vec2 centered = p_w - half_size;
        vec2 d = abs(centered) - half_size + radius;
        float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
        if (sdf > 0.0) return false;
    }

    hit.t = t;
    hit.uv = vec2(s, tt);
    hit.normal = n;
    return true;
}

// Ray vs. oriented box. Origin is one corner; u/v/w are three edges, whose length
// carries the extent. Uses a slab test in the box's frame. Normal is the face
// normal of whichever slab bounded the intersection. UV is the hit point projected
// into the 2D coordinates of the hit face.
bool intersect_cube(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 U = shape.u_axis.xyz;
    vec3 V = shape.v_axis.xyz;
    vec3 W = shape.w_axis.xyz;
    float u_len2 = dot(U, U);
    float v_len2 = dot(V, V);
    float w_len2 = dot(W, W);
    if (u_len2 < 1e-12 || v_len2 < 1e-12 || w_len2 < 1e-12) return false;

    // The ray in the box's frame: corner at 0, axes aligned to U/V/W.
    vec3 rel = ray.origin - shape.origin.xyz;
    vec3 ro_l = vec3(dot(rel, U) / u_len2,
                     dot(rel, V) / v_len2,
                     dot(rel, W) / w_len2);
    vec3 rd_l = vec3(dot(ray.dir, U) / u_len2,
                     dot(ray.dir, V) / v_len2,
                     dot(ray.dir, W) / w_len2);

    // Slab test against [0, 1]^3, since U/V/W are full extents. No direction component exactly
    // zero: a ray parallel to a face and starting on its plane would make 0 times infinity.
    vec3 inv_d = 1.0 / mix(rd_l, sign(rd_l + vec3(1e-30)) * 1e-12, lessThan(abs(rd_l), vec3(1e-12)));
    vec3 t0 = (vec3(0.0) - ro_l) * inv_d;
    vec3 t1 = (vec3(1.0) - ro_l) * inv_d;
    vec3 tmin_v = min(t0, t1);
    vec3 tmax_v = max(t0, t1);
    float tmin = max(max(tmin_v.x, tmin_v.y), tmin_v.z);
    float tmax = min(min(tmax_v.x, tmax_v.y), tmax_v.z);
    if (tmax < max(tmin, 0.0)) return false;

    float t = tmin > 0.0 ? tmin : tmax;
    if (t <= 0.0) return false;

    // Which axis's slab bounded tmin (or tmax if we're inside).
    vec3 chosen = tmin > 0.0 ? tmin_v : tmax_v;
    float chosen_t = tmin > 0.0 ? tmin : tmax;
    // Outward, whichever side the ray is on: against the ray going in, along it coming out.
    float going = tmin > 0.0 ? -1.0 : 1.0;
    vec3 n_l;
    vec2 face_uv;
    vec3 p_l = ro_l + chosen_t * rd_l;
    if (chosen.x == chosen_t) {
        n_l = vec3(rd_l.x > 0.0 ? going : -going, 0.0, 0.0);
        face_uv = vec2(p_l.y, p_l.z);
    } else if (chosen.y == chosen_t) {
        n_l = vec3(0.0, rd_l.y > 0.0 ? going : -going, 0.0);
        face_uv = vec2(p_l.x, p_l.z);
    } else {
        n_l = vec3(0.0, 0.0, rd_l.z > 0.0 ? going : -going);
        face_uv = vec2(p_l.x, p_l.y);
    }

    vec3 n = normalize(n_l.x * U + n_l.y * V + n_l.z * W);

    hit.t = t;
    hit.uv = clamp(face_uv, 0.0, 1.0);
    hit.normal = n;
    return true;
}

// Ray vs. sphere, centered at origin + (u + v + w) * 0.5 with radius params.x.
bool intersect_sphere(Ray ray, RtShape shape, out RayHit hit)
{
    vec3 center = shape.origin.xyz
                + 0.5 * (shape.u_axis.xyz + shape.v_axis.xyz + shape.w_axis.xyz);
    float radius = shape.params.x;
    if (radius <= 0.0) return false;

    // Solved from the ray's closest point to the center, so a distant origin does
    // not cost precision.
    vec3 oc = ray.origin - center;
    float a = dot(ray.dir, ray.dir);
    float t_closest = -dot(oc, ray.dir) / a;
    vec3 closest = oc + t_closest * ray.dir;
    float h2 = (radius * radius - dot(closest, closest)) / a;
    if (h2 < 0.0) return false;
    float h = sqrt(h2);
    float t1 = t_closest - h;
    float t2 = t_closest + h;
    float t = t1 > 0.0 ? t1 : t2;
    if (t <= 0.0) return false;

    vec3 n = normalize(closest + (t - t_closest) * ray.dir);

    // Spherical equirect-style UV.
    float u = atan(n.z, n.x) / (2.0 * 3.14159265) + 0.5;
    float v = asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5;

    hit.t = t;
    hit.uv = vec2(u, v);
    hit.normal = n;
    return true;
}
