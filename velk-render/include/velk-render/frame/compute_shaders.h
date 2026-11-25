#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/frame/light_pool.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/frame/ray_walk.h>

#include <cstdio>
#include <cstring>

namespace velk {

// Raster default shaders + driver templates + compose_eval_fragment
// live in <velk-render/frame/raster_shaders.h>. This file holds the
// scene-side shader bits: the velk-ui GLSL include, the deferred-lighting
// compute shader, and the RT compute prelude/main.

// Rays and the built-in shape intersects, shared by every compute shader that
// traces. Registered as the "velk_intersect.glsl" include. The including shader
// declares RtShape through velk.glsl.
[[maybe_unused]] constexpr string_view velk_intersect_src = R"(
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
)";

/**
 * Fills the G-buffer from the visibility buffer: for each pixel, finds its triangle, where
 * the view ray meets it, and what the material gives there; and the pixel's record, its ids
 * and weights in its triangle (velk_pixel_ids, velk_pixel_weights). Goes after
 * compose_rt_compute's shared body, whose push constants it borrows.
 */
inline constexpr string_view visibility_resolve_main_src = R"(
layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];

#define RESOLVE_IDS pc.extras.x
#define RESOLVE_WIDTH pc.extras.y
#define RESOLVE_HEIGHT pc.extras.z
#define RESOLVE_ALBEDO pc.extras.w
#define RESOLVE_NORMAL pc.env.x
#define RESOLVE_POSITION pc.env.y
#define RESOLVE_MATERIAL pc.env.z
#define RESOLVE_EMISSIVE pc.env.w
#define RESOLVE_SPECULAR pc.more.x
// The resolve has no lights, so their count carries the word of the depth plane that compute raster drew to, a word a pixel
// (instance_tiny_draw_src). After it come the pixels' ids, two words a pixel
// (velk_pixel_ids): compute raster's where it drew, the hardware's written here elsewhere.
#define RESOLVE_TINY pc.light_count

layout(set = 1, binding = 16, std430) buffer ResolveWords { uint data[]; } resolve_words;

// Where the view ray through a point of the screen, in pixels, meets a triangle's plane. A
// ray rather than dividing by each corner's w, which fails for corners behind the camera. It
// starts on the near plane so that it also suits orthographic views. Leaves the results
// alone for a ray along the plane.
void resolve_hit(GlobalData g, vec2 pixel, vec3 p0, vec3 e1, vec3 e2, inout vec3 weights, inout vec3 point)
{
    vec2 ndc = pixel / vec2(float(RESOLVE_WIDTH), float(RESOLVE_HEIGHT)) * 2.0 - 1.0;
    vec4 near_point = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec4 far_point = g.inverse_view_projection * vec4(ndc, 1e-4, 1.0);
    vec3 origin = near_point.xyz / near_point.w;
    vec3 dir = normalize(far_point.xyz / far_point.w - origin);

    vec3 h = cross(dir, e2);
    float det = dot(e1, h);
    if (abs(det) > 0.0) {
        vec3 s = origin - p0;
        vec3 q = cross(s, e1);
        float b1 = dot(s, h) / det;
        float b2 = dot(dir, q) / det;
        weights = vec3(1.0 - b1 - b2, b1, b2);
        // Along the ray, so that imprecision in a large triangle's weights only moves
        // the point towards or away from the camera.
        point = origin + dir * (dot(e2, q) / det);
    }
}

vec3 resolve_shading_normal(MaterialEval e, EvalContext ctx)
{
    return normalize(length(e.normal) > 0.0 ? e.normal : ctx.normal);
}

)" R"(
#extension GL_KHR_shader_subgroup_quad : require

// A group's 64 threads as 16 blocks of 2x2 pixels, a subgroup quad each, so that a pixel
// reads its neighbours across and down with quad swaps.
ivec2 resolve_pixel()
{
    uint linear = gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID;
    uint quad = linear >> 2u;
    uint lane = linear & 3u;
    return ivec2(gl_WorkGroupID.xy) * 8 + ivec2((quad & 3u) * 2u + (lane & 1u), (quad >> 2u) * 2u + (lane >> 1u));
}

void main()
{
    // Every thread goes on to the quad swaps, pixels off the image and empty ones too.
    ivec2 coord = resolve_pixel();
    bool inside = coord.x < int(RESOLVE_WIDTH) && coord.y < int(RESOLVE_HEIGHT);

    // Compute raster drew here, nearer than the hardware, and left its ids. Its depth is
    // cleared for the next frame.
    uvec2 ids = uvec2(0u);
    uint ids_at = 0u;
    if (inside) {
        uint pixel_index = uint(coord.y) * RESOLVE_WIDTH + uint(coord.x);
        ids_at = RESOLVE_TINY + RESOLVE_WIDTH * RESOLVE_HEIGHT + pixel_index * VELK_PIXEL_RECORD_WORDS;
        bool tiny = resolve_words.data[RESOLVE_TINY + pixel_index] != 0u;
        if (tiny) {
            resolve_words.data[RESOLVE_TINY + pixel_index] = 0u;
            ids = uvec2(resolve_words.data[ids_at], resolve_words.data[ids_at + 1u]);
        } else {
            ids = texelFetch(velk_textures_uint[nonuniformEXT(RESOLVE_IDS)], coord, 0).xy;
            resolve_words.data[ids_at] = ids.x;
            resolve_words.data[ids_at + 1u] = ids.y;
        }
        // The culling's trace (InstanceCullPass::trace), at its pixel: which raster's ids were
        // taken, and what they are.
        uint trace = VELK_GLOBALS.trace;
        if (trace != 0xFFFFFFFFu && uint(coord.x) == resolve_words.data[trace] && uint(coord.y) == resolve_words.data[trace + 1u]) {
            uint at = atomicAdd(resolve_words.data[trace + 2u], 1u);
            if (at < resolve_words.data[trace + 3u]) {
                uint w = trace + 6u + at * 4u;
                resolve_words.data[w] = 5u | ((tiny ? 1u : 0u) << 8u);
                resolve_words.data[w + 1u] = ids.x;
                resolve_words.data[w + 2u] = ids.y;
                resolve_words.data[w + 3u] = 0u;
            }
        }
    }
    bool drawn = ids.x != 0u;

    MaterialEval e = velk_default_material_eval();
    vec3 N = vec3(0.0);
    vec3 smooth_normal = vec3(0.0);
    vec3 rel = vec3(0.0);
    float lod_error = 0.0;
    float tri_size = 0.0;
    // A stand-in's flatness (velk_stand_in_flatness): a quad of a view's quads, or a quad standing
    // for small pieces; below 0 for a surface as drawn.
    float stand_in = -1.0;
    // The draw and instance, which a neighbour must share to count.
    uvec2 surface = uvec2(0xFFFFFFFFu);
    // Seen from both sides: only such a thin sheet passes light through (MaterialEval::transmission).
    bool two_sided = false;
    if (drawn) {
        VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
        two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        uint triangle = velk_ids_triangle(d, ids);
        uint slot = velk_ids_slot(d, ids);
        ElementInstance inst = velk_draw_instance(d, slot);
        GlobalData g = velk_globals.data[d.globals_base];
        surface = uvec2(velk_ids_draw(ids), slot);

        uint cluster = velk_ids_cluster(d, ids);
        uvec3 corners = velk_surface_corners(d, cluster, triangle);
        VelkSurfaceVertex v0 = velk_surface_vertex(d, cluster, inst, g, corners.x);
        VelkSurfaceVertex v1 = velk_surface_vertex(d, cluster, inst, g, corners.y);
        VelkSurfaceVertex v2 = velk_surface_vertex(d, cluster, inst, g, corners.z);

        vec3 e1 = v1.rel - v0.rel;
        vec3 e2 = v2.rel - v0.rel;
        vec2 pixel = vec2(coord) + 0.5;
        vec3 weights = vec3(1.0 / 3.0);
        rel = (v0.rel + v1.rel + v2.rel) / 3.0;
        resolve_hit(g, pixel, v0.rel, e1, e2, weights, rel);

        // For the passes after, which read the pixel's light: where it is in its triangle
        // (velk_pixel_weights), and the triangle's longest edge.
        vec3 b = clamp(weights, 0.0, 1.0);
        b /= max(b.x + b.y + b.z, 1e-6);
        resolve_words.data[ids_at + 2u] = floatBitsToUint(b.y);
        resolve_words.data[ids_at + 3u] = floatBitsToUint(b.z);
        vec3 e3 = v2.rel - v1.rel;
        tri_size = sqrt(max(max(dot(e1, e1), dot(e2, e2)), dot(e3, e3)));

        // Texture coordinates' steps to the next pixels along, on this triangle's plane even
        // where another covers them.
        vec3 weights_x = weights;
        vec3 weights_y = weights;
        vec3 rel_x = rel;
        vec3 rel_y = rel;
        resolve_hit(g, pixel + vec2(1.0, 0.0), v0.rel, e1, e2, weights_x, rel_x);
        resolve_hit(g, pixel + vec2(0.0, 1.0), v0.rel, e1, e2, weights_y, rel_y);
        mat3x2 uvs = mat3x2(v0.uv, v1.uv, v2.uv);
        mat3x2 uv1s = mat3x2(v0.uv1, v1.uv1, v2.uv1);
        velk_uv_dx[0] = uvs * (weights_x - weights);
        velk_uv_dy[0] = uvs * (weights_y - weights);
        velk_uv_dx[1] = uv1s * (weights_x - weights);
        velk_uv_dy[1] = uv1s * (weights_y - weights);

        EvalContext ctx = velk_surface_context(d, g, v0, v1, v2, weights, rel);
        velk_uv_set = 0u;
        e = velk_eval_material(d.material_id, ctx);
        N = resolve_shading_normal(e, ctx);
        smooth_normal = ctx.normal;
        // Seen from both sides: the side it shows is the one toward the camera, of its plane as
        // drawn (whatever its winding), and its normal turns to that side. Which side its normals
        // are on is its vertex normal's, so that a normal map cannot tip it.
        if (two_sided) {
            vec3 plane = cross(e1, e2);
            if (dot(smooth_normal, plane) * dot(-rel, plane) < 0.0) {
                N = -N;
                smooth_normal = -smooth_normal;
            }
        }
        // A quad of a view's quads by its node's surface, else by its normals' length, which only a
        // level of detail's data (a cluster's) keeps for it.
        if ((d.flags & VELK_DRAW_QUADS) != 0u) {
            stand_in = velk_surface_flatness(inst.params.y);
            e.roughness = velk_surface_roughness(inst.params.y);
        } else if (cluster != VELK_NO_CLUSTER) {
            stand_in = velk_stand_in_flatness(dot(b, vec3(v0.normal_length, v1.normal_length, v2.normal_length)));
        }

        mat3 world_axes = mat3(inst.world_matrix);
        lod_error = velk_cluster_error(cluster, max(max(length(world_axes[0]), length(world_axes[1])), length(world_axes[2])));
    }

    // Widens roughness where the shading normal changes quickly between pixels, so that a
    // tight highlight does not alias (Tokuyoshi 2019). A neighbour counts on the same draw and
    // instance with a smooth normal close to this pixel's, so that an edge between surfaces
    // does not; without one across or down, the other stands in for it.
    vec3 n_x = subgroupQuadSwapHorizontal(N);
    vec3 n_y = subgroupQuadSwapVertical(N);
    vec3 smooth_x = subgroupQuadSwapHorizontal(smooth_normal);
    vec3 smooth_y = subgroupQuadSwapVertical(smooth_normal);
    uvec2 surface_x = subgroupQuadSwapHorizontal(surface);
    uvec2 surface_y = subgroupQuadSwapVertical(surface);
    if (!drawn) {
        if (inside) {
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, vec4(0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(0.0));
            imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(0.0));
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord, vec4(0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(0.0));
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_SPECULAR)], coord, vec4(0.0));
        }
        return;
    }
    bool across = surface_x == surface && dot(smooth_x, smooth_normal) > 0.9;
    bool down = surface_y == surface && dot(smooth_y, smooth_normal) > 0.9;
    vec3 dndx = across ? n_x - N : vec3(0.0);
    vec3 dndy = down ? n_y - N : vec3(0.0);
    if (!across) dndx = dndy;
    if (!down) dndy = dndx;
    float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
    float roughness = sqrt(clamp(e.roughness * e.roughness + min(2.0 * variance, 0.18), 0.0, 1.0));

    imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, e.color);
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(N, lod_error));
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(rel, tri_size));
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord,
               vec4(e.metallic, roughness, float(e.lighting_mode) / 255.0, velk_stand_in_pack(stand_in)));
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(e.emissive, two_sided ? clamp(e.transmission, 0.0, 1.0) : 0.0));
    // As a dielectric (KHR_materials_specular, velk_specular_f0): the square root of its
    // reflectance at normal incidence, for steps fine near 0.04 and room up to 1, and its
    // reflectance at grazing.
    float specular = clamp(e.specular_factor, 0.0, 1.0);
    vec3 dielectric_f0 = min(0.04 * max(e.specular_color_factor, vec3(0.0)), vec3(1.0)) * specular;
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_SPECULAR)], coord, vec4(sqrt(dielectric_f0), specular));
}
)";

/// Draws the edges of the view of the camera that chooses what a view draws (ICamera::choose_as)
/// over that view's image: its four sides out to `reach` world units, and the rectangle
/// there. A thread per point along an edge, kChooserEdgeSteps of them per edge, 8 edges.
inline constexpr uint32_t kChooserEdgeSteps = 4096;
inline constexpr string_view chooser_frustum_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint chooser_globals;
    float reach;
} pc;

const uint kSteps = 4096u;

// The direction through a corner of the chooser's view, in world axes.
vec3 corner_direction(GlobalData chooser, vec2 ndc)
{
    vec4 p = chooser.inverse_view_projection * vec4(ndc, 0.5, 1.0);
    return normalize(p.xyz / p.w);
}

void main()
{
    uint edge = gl_GlobalInvocationID.x / kSteps;
    if (edge >= 8u) return;
    float t = float(gl_GlobalInvocationID.x % kSteps) / float(kSteps - 1u);

    GlobalData view = velk_globals.data[pc.view_globals];
    GlobalData chooser = velk_globals.data[pc.chooser_globals];
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    // The chooser's position from this view's camera.
    vec3 eye = chooser.cam_pos.xyz - view.cam_pos.xyz;
    vec3 point;
    if (edge < 4u) {
        point = eye + corner_direction(chooser, corners[edge]) * (pc.reach * t);
    } else {
        vec3 a = eye + corner_direction(chooser, corners[edge - 4u]) * pc.reach;
        vec3 b = eye + corner_direction(chooser, corners[(edge - 3u) & 3u]) * pc.reach;
        point = mix(a, b, t);
    }
    vec4 clip = view.view_projection * vec4(point, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 at = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(at, vec2(0.0))) || at.x >= float(pc.width) || at.y >= float(pc.height)) return;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(at), vec4(1.0, 1.0, 0.0, 1.0));
}
)";

/// Draws the bounds of a visual's partitions over a view's image (DebugView::Partition): red for
/// a partition that chooses its levels of detail again this frame, cyan for one that keeps
/// them. `table_word` is the visual's partition table (kCullEntryWords a partition), `cut_word`
/// its kept cut in this view (kCutHeaderWords). A thread per point along an edge,
/// kPartitionEdgeSteps of them per edge, 12 edges a partition.
inline constexpr uint32_t kPartitionEdgeSteps = 256;
inline constexpr string_view partition_bounds_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint table_word;
    uint partition_count;
    uint cut_word;
} pc;

const uint kSteps = 256u;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    uint partition_index = index / (12u * kSteps);
    if (partition_index >= pc.partition_count) return;
    uint edge = (index / kSteps) % 12u;
    float t = float(index % kSteps) / float(kSteps - 1u);

    uint entry = pc.table_word + partition_index * VELK_CULL_ENTRY_WORDS;
    if (velk_culling.data[entry + VELK_PARTITION_ENTRY_CHUNKS] == 0u) return;
    vec3 lo = vec3(uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MIN]),
                   uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MIN + 1u]),
                   uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MIN + 2u]));
    vec3 hi = vec3(uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MAX]),
                   uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MAX + 1u]),
                   uintBitsToFloat(velk_culling.data[entry + VELK_PARTITION_ENTRY_MAX + 2u]));
    uint state = velk_culling.data[pc.cut_word + VELK_CUT_STATES_WORD] + partition_index * VELK_CUT_PARTITION_WORDS;
    bool choosing = velk_culling.data[state + VELK_PART_CHOOSING] != 0u;

    // Edges 0..3 along x, 4..7 along y, 8..11 along z, at the four corners of the other two.
    uint axis = edge / 4u;
    uint corner = edge % 4u;
    vec3 other = vec3((corner & 1u) != 0u ? 1.0 : 0.0, (corner & 2u) != 0u ? 1.0 : 0.0, 0.0);
    vec3 at;
    if (axis == 0u) at = vec3(t, other.x, other.y);
    else if (axis == 1u) at = vec3(other.x, t, other.y);
    else at = vec3(other.x, other.y, t);
    vec3 point = mix(lo, hi, at);

    GlobalData view = velk_globals.data[pc.view_globals];
    vec4 clip = view.view_projection * vec4(point - view.cam_pos.xyz, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 pixel = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(pixel, vec2(0.0))) || pixel.x >= float(pc.width) || pixel.y >= float(pc.height)) return;
    vec3 color = choosing ? vec3(1.0, 0.15, 0.1) : vec3(0.1, 0.8, 0.9);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(pixel), vec4(color, 1.0));
}
)";

/// Draws the boxes of one level of the trees over a kept cut's partitions (DebugView::RayTree)
/// over a view's image: at `level` k the children of the nodes of height k + 1, at 0 leaves of
/// four records, and a partition's root where its tree is not that tall, coloured by level. `cut_word` is
/// the kept cut (kCutHeaderWords). With `top_word` set instead, the view's top tree over the
/// partitions (kRayTopHeaderWords, DebugView::RayTop): at 0 each partition's box. A thread per
/// point along an edge, kRayTreeEdgeSteps of them per edge, 12 edges a box, two boxes a node.
inline constexpr uint32_t kRayTreeEdgeSteps = 32;
inline constexpr string_view ray_tree_bounds_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint cut_word;
    uint level;
    uint top_word;
} pc;

const uint kSteps = 32u;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    uint node_index = index / (24u * kSteps);
    uint side = (index / (12u * kSteps)) % 2u;
    uint edge = (index / kSteps) % 12u;
    float t = float(index % kSteps) / float(kSteps - 1u);

    bool top = pc.top_word != 0xFFFFFFFFu;
    uint node;
    if (top) {
        uint cuts = velk_culling.data[pc.top_word + VELK_RAY_TOP_CUTS];
        uint leaves = velk_culling.data[pc.top_word + VELK_RAY_TOP_LEAVES];
        if (node_index >= velk_culling.data[pc.top_word + VELK_RAY_TOP_NODES]) return;
        node = pc.top_word + VELK_RAY_TOP_HEADER_WORDS + cuts * VELK_RAY_TOP_CUT_WORDS + leaves * VELK_RAY_TOP_LEAF_WORDS
             + node_index * VELK_CUT_NODE_WORDS;
    } else {
        // The pool's nodes up to the last page in use; only those of the tree of the partition
        // that holds the page.
        uint cut = pc.cut_word;
        uint page = node_index / VELK_CUT_PAGE_NODES;
        if (page >= velk_culling.data[cut + VELK_CUT_DRAW_GROUPS] || page >= velk_culling.data[cut + VELK_CUT_PAGES]) return;
        uint header = velk_culling.data[cut + VELK_CUT_PAGE_HEADERS] + page * VELK_CUT_PAGE_WORDS;
        uint owner = velk_culling.data[header + VELK_CUT_PAGE_OWNER];
        if (owner >= velk_culling.data[cut + VELK_CUT_PARTITIONS]) return;
        uint state = velk_culling.data[cut + VELK_CUT_STATES_WORD] + owner * VELK_CUT_PARTITION_WORDS;
        uint rays = velk_culling.data[state + VELK_PART_RAY_COUNT];
        uint tree_nodes = rays == 0u ? 0u : max((rays + 3u) / 4u, 2u) - 1u;
        if (velk_culling.data[header + VELK_CUT_PAGE_PLACE] * VELK_CUT_PAGE_NODES + node_index % VELK_CUT_PAGE_NODES >= tree_nodes) return;
        node = velk_culling.data[cut + VELK_CUT_NODE_POOL] + node_index * VELK_CUT_NODE_WORDS;
    }
    uint level = velk_culling.data[node + VELK_CUT_NODE_LEVEL];
    bool root = velk_culling.data[node + VELK_CUT_NODE_ROOT] != 0u;
    // A node's children are a level below it; a short tree shows its root's.
    if (level != pc.level + 1u && !(root && level < pc.level + 1u)) return;
    if (velk_culling.data[node + VELK_CUT_NODE_LEFT_REF + side] == 0xFFFFFFFFu) return;
    uint box = node + VELK_CUT_NODE_LEFT_MIN + side * (VELK_CUT_NODE_RIGHT_MIN - VELK_CUT_NODE_LEFT_MIN);
    vec3 lo = vec3(uintBitsToFloat(velk_culling.data[box]), uintBitsToFloat(velk_culling.data[box + 1u]),
                   uintBitsToFloat(velk_culling.data[box + 2u]));
    vec3 hi = vec3(uintBitsToFloat(velk_culling.data[box + 3u]), uintBitsToFloat(velk_culling.data[box + 4u]),
                   uintBitsToFloat(velk_culling.data[box + 5u]));
    if (any(greaterThan(lo, hi))) return;

    // Edges 0..3 along x, 4..7 along y, 8..11 along z, at the four corners of the other two.
    uint axis = edge / 4u;
    uint corner = edge % 4u;
    vec2 other = vec2((corner & 1u) != 0u ? 1.0 : 0.0, (corner & 2u) != 0u ? 1.0 : 0.0);
    vec3 at;
    if (axis == 0u) at = vec3(t, other.x, other.y);
    else if (axis == 1u) at = vec3(other.x, t, other.y);
    else at = vec3(other.x, other.y, t);
    vec3 point = mix(lo, hi, at);

    GlobalData view = velk_globals.data[pc.view_globals];
    vec4 clip = view.view_projection * vec4(point - view.cam_pos.xyz, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 pixel = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(pixel, vec2(0.0))) || pixel.x >= float(pc.width) || pixel.y >= float(pc.height)) return;
    // Hue by the level the box is at, so roots of short trees stand apart.
    float shown = float(level - 1u);
    vec3 color = 0.55 + 0.45 * cos(6.2831853 * (shown * 0.17 + vec3(0.0, 0.33, 0.67)));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(pixel), vec4(color, 1.0));
}
)";

/// Shows the visibility buffer as flat colours: mode 1 per draw, 2 per instance, 3 per
/// triangle. Mode 4 shows the depth pyramid's fourth level.
/// Modes 5 and 6 show what compute drew and how large triangles are on screen
/// (triangle_size_color). Modes 7 to 9 show level of detail clusters (cluster_color): one
/// colour per cluster of each instance, by level, and by width on screen as the camera that
/// chooses sees it (FrameGlobals at chooser_globals). Modes 10 to 12 show instances dimmed,
/// for partition_bounds_compute_src and ray_tree_bounds_compute_src to draw over. With `dim_outside`, clusters outside the
/// chooser's view are dimmed. The numbers are DebugView's.
inline constexpr string_view visibility_debug_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint ids_word;
    uint output_id;
    uint width;
    uint height;
    uint mode;
    uint pyramid_word;
    uint chooser_globals;
    uint dim_outside;
    uint pool_word;       // the view's light pool (light_pool.h), for mode 15
    float light_pixels;   // kLightPixels
} pc;

#define VELK_LIGHT_WORDS velk_culling
)" VELK_LIGHT_POOL_GLSL R"(
vec3 id_color(uint id)
{
    uint h = (id + 1u) * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

// A colour per kept cut record by what it is (entry, cluster, slot), which stays from frame to
// frame, also when the record's partition chooses again and its place in the cut's pool moves.
vec3 record_color(uint record)
{
    return id_color(velk_culling.data[record + VELK_RECORD_ENTRY] * 2654435761u
                    ^ velk_culling.data[record + VELK_RECORD_CLUSTER] * 2246822519u
                    ^ velk_culling.data[record + VELK_RECORD_SLOT] * 3266489917u);
}

struct DebugInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};
layout(set = 1, binding = 3, scalar) readonly buffer DebugInstances { DebugInstance data[]; } debug_instances;

// Mode 5: a pixel compute drew magenta, the rest grey. Mode 6: compute's magenta, the rest by
// how large their triangle is on screen, the square root of its area in pixels: under a half
// red, under 1 yellow, under 2 green, under 8 cyan, and larger blue.
vec3 triangle_size_color(uvec2 ids)
{
    if ((ids.x & VELK_IDS_TINY) != 0u) return vec3(1.0, 0.0, 1.0);
    if (pc.mode == 5u) return vec3(0.3);
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    // A quad's slot is its word, not an instance this view's instances hold.
    if ((d.flags & VELK_DRAW_QUADS) != 0u) return vec3(0.25);
    uint triangle = velk_ids_triangle(d, ids);
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    GlobalData g = velk_globals.data[d.globals_base];
    uvec3 corners = uvec3(triangle) + uvec3(0u, 1u, 2u);
    uint cluster = velk_ids_cluster(d, ids);
    if (d.index_base != VELK_NO_VISIBLE_LIST) {
        uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
        corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    }
    vec2 p[3];
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        vec4 clip = g.view_projection * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), 1.0);
        if (clip.w <= 0.0) return vec3(0.0, 0.0, 1.0);
        p[k] = clip.xy / clip.w * 0.5 * vec2(float(pc.width), float(pc.height));
    }
    vec2 e1 = p[1] - p[0];
    vec2 e2 = p[2] - p[0];
    float across = sqrt(0.5 * abs(e1.x * e2.y - e1.y * e2.x));
    return across < 0.5 ? vec3(1.0, 0.0, 0.0)
         : across < 1.0 ? vec3(1.0, 1.0, 0.0)
         : across < 2.0 ? vec3(0.0, 1.0, 0.0)
         : across < 8.0 ? vec3(0.0, 1.0, 1.0)
         : vec3(0.0, 0.0, 1.0);
}

// Mode 15: where light comes from. A triangle with pieces: the depth of the piece a pixel reads
// (light_color says the colours). Else a record with points: a colour per point of light, blended
// as a pixel reads them (its triangle's corners), so each patch is one point's reach; one with one
// value flat in its colour; a quad
// in the colour of the node its light is on; magenta as far as that light is not traced yet; what
// has no light yet grey, and what is from no record white.
vec3 light_color(uvec2 ids, ivec2 coord)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return vec3(1.0);
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    if (velk_culling.data[record + VELK_RECORD_ENTRY] == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(velk_culling.data[record + VELK_RECORD_QUAD_TREE],
                                         velk_culling.data[record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz, pixels_per_unit,
                                         pc.light_pixels);
        uint value = velk_culling.data[node + VELK_QUAD_NODE_LIGHT];
        if (value == VELK_LIGHT_NONE) return vec3(0.25);
        return (value & VELK_LIGHT_ONE_TRACED) == 0u ? vec3(1.0, 0.0, 1.0) : id_color(node);
    }
    uint light = velk_culling.data[record + VELK_RECORD_LIGHT];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return vec3(0.25);
    if (light >= VELK_LIGHT_ONE_VALUE) {
        return (light & VELK_LIGHT_ONE_TRACED) == 0u ? vec3(1.0, 0.0, 1.0) : record_color(record) * 0.8;
    }
    if (pc.pool_word == 0xFFFFFFFFu || light >= velk_culling.data[pc.pool_word + VELK_POOL_PAGES]) return vec3(1.0, 0.0, 0.0);
    uint first = velk_light_page(pc.pool_word, light);
    vec3 base = record_color(record) * 0.35;

    // The pixel's triangle on screen, in pixels.
    uint triangle = velk_ids_triangle(d, ids);
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return base;
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    vec3 p[3];
    vec2 size = vec2(float(pc.width), float(pc.height));
    for (int k = 0; k < 3; ++k) {
        uint at = velk_cluster_vertex_word(d, cluster) + corners[k] * VELK_VERTEX3D_WORDS;
        vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                          uintBitsToFloat(velk_mesh_words.data[at + 2u]));
        p[k] = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    }
    // Its weights there as the resolve left them for the claim and the lighting; where the
    // pixel's view ray meets the triangle's plane, for how many pixels a world unit is there.
    vec3 b = velk_pixel_weights(pc.ids_word, pc.width, coord);
    vec3 e1 = p[1] - p[0];
    vec3 e2 = p[2] - p[0];
    vec2 ndc = (vec2(coord) + 0.5) / size * 2.0 - 1.0;
    vec4 on_ray = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec3 dir = on_ray.xyz / on_ray.w;
    vec3 normal = cross(e1, e2);
    float facing = dot(normal, dir);
    if (abs(facing) < 1e-20) return base;
    float d11 = dot(e1, e1);
    float d22 = dot(e2, e2);
    // In pieces: the level the pixel reads as the lighting finds it (its coarse level where that
    // level's cell is exact), coloured by its depth (red at the root, then orange,
    // yellow, green, cyan, blue, purple, pink, again from red; dark grey its triangle's vertex
    // points), darker toward its points' places so that its grid shows; magenta as far as the
    // points it reads are not traced yet, cyan as far as they are stale since a change.
    float tri_size = sqrt(max(max(d11, d22), dot(p[2] - p[1], p[2] - p[1])));
    VelkLightRoot r = velk_light_root_of(first, triangle, b, tri_size);
    if (r.slot != VELK_LIGHT_NONE) {
        const vec3 kDepthColors[8] = vec3[8](
            vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
            vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));
        uint root = velk_culling.data[velk_light_root_word(pc.pool_word, first, r.slot)];
        vec3 at = dir * (dot(normal, p[0]) / facing);
        float px_per_world = pixels_per_unit / max(length(at), 1e-4);
        float tri_px = tri_size * px_per_world;
        VelkPieceWay way = velk_piece_way(pc.pool_word, root, b, tri_px, r, r.size * px_per_world, pc.light_pixels);
        if (velk_way_cell(pc.pool_word, first, corners, way, true) == 1u) velk_way_stop_at_coarse(way);
        uint words[3];
        vec3 w;
        velk_way_around(pc.pool_word, first, corners, way.piece, way.at, words, w);
        float untraced = 0.0;
        float stale = 0.0;
        for (uint k = 0u; k < 3u; ++k) {
            if (velk_culling.data[words[k]] == VELK_LIGHT_NONE) untraced += w[k];
            else if (velk_light_point_traces(pc.pool_word, first, way.piece == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.piece,
                                             velk_culling.data[words[k] + 1u]) == 0u) {
                stale += w[k];
            }
        }
        vec3 shown = way.piece == VELK_PIECE_VERTICES ? vec3(0.3) : kDepthColors[way.depth & 7u];
        vec3 color = shown * (0.55 + 0.45 * max(max(w.x, w.y), w.z));
        color = mix(color, vec3(0.0, 1.0, 1.0), clamp(stale, 0.0, 1.0));
        return mix(color, vec3(1.0, 0.0, 1.0), clamp(untraced, 0.0, 1.0));
    }
    vec3 color = vec3(0.0);
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pc.pool_word, first, corners, b, words, points, weights, owners);
    float untrusted = 0.0;
    float retracing = 0.0;
    for (uint k = 0u; k < read; ++k) {
        color += weights[k] * id_color(words[k] * 2654435761u);
        uint traces = velk_light_point_traces(pc.pool_word, owners[k], VELK_LIGHT_NONE, velk_culling.data[words[k] + 1u]);
        bool none = velk_culling.data[words[k]] == VELK_LIGHT_NONE;
        float trust = none ? 0.0 : float(traces);
        untrusted += weights[k] * (1.0 - trust);
        if (!none && traces == 0u) retracing += weights[k];
    }
    color *= 0.45;
    // Magenta as far as the points it reads are not traced yet, cyan as far as they are stale
    // since a change.
    color = mix(color, vec3(0.0, 1.0, 1.0), clamp(retracing, 0.0, 1.0));
    return mix(color, vec3(1.0, 0.0, 1.0), clamp(untrusted, 0.0, 1.0));
}

)" R"(
const vec3 kLevelColors[8] = vec3[8](
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 1.0, 1.0), vec3(0.0, 0.3, 1.0), vec3(0.6, 0.0, 1.0), vec3(1.0, 0.0, 0.7));

// Mode 7: a colour per cluster of each instance. Mode 8: by the cluster's level, full detail
// red, then orange, yellow, green, cyan, blue, purple, pink. Mode 9: by the width on screen of
// the sphere around the cluster's own triangles: under 8 pixels blue, under 16 cyan, under 32
// green, under 64 yellow, under 128 orange, larger red. What is not drawn by cluster is grey.
vec3 cluster_color(uvec2 ids)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return vec3(0.25);
    uint slot = velk_ids_slot(d, ids);
    if (pc.mode == 7u) return id_color(cluster * 7919u + slot * 104729u);
    if (pc.mode == 8u) return kLevelColors[min(velk_culling.data[cluster + VELK_LOD_CLUSTER_LEVEL], 7u)];

    DebugInstance inst = debug_instances.data[d.instances_base + slot];
    GlobalData g = velk_globals.data[pc.chooser_globals];
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    vec3 center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    float across = 2.0 * radius * pixels_per_unit / max(length(center) - radius, 1e-3);
    return across < 8.0 ? vec3(0.0, 0.0, 1.0)
         : across < 16.0 ? vec3(0.0, 1.0, 1.0)
         : across < 32.0 ? vec3(0.0, 1.0, 0.0)
         : across < 64.0 ? vec3(1.0, 1.0, 0.0)
         : across < 128.0 ? vec3(1.0, 0.5, 0.0)
         : vec3(1.0, 0.0, 0.0);
}

// Whether the cluster a pixel shows is outside the view of the camera that chose it.
bool outside_chooser(uvec2 ids)
{
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return false;
    DebugInstance inst = debug_instances.data[d.instances_base + velk_ids_slot(d, ids)];
    GlobalData g = velk_globals.data[pc.chooser_globals];
    vec3 local = vec3(uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                      uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    mat3 to_world = mat3(inst.world_matrix);
    float scale = max(max(length(to_world[0] * inst.size.x), length(to_world[1] * inst.size.y)), length(to_world[2] * inst.size.z));
    float radius = uintBitsToFloat(velk_culling.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    vec3 center = velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g);
    mat4 vp = g.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    return abs(clip.x) > clip.w + w_reach + radius * length(r0) || abs(clip.y) > clip.w + w_reach + radius * length(r1)
        || abs(clip.z) > clip.w + w_reach + radius * length(r2);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    if (pc.mode == 4u) {
        const uint level = 3u;
        uint width = max(velk_culling.data[pc.pyramid_word + 1u] >> level, 1u);
        uint height = max(velk_culling.data[pc.pyramid_word + 2u] >> level, 1u);
        uint first = velk_culling.data[pc.pyramid_word + 3u + level];
        uvec2 at = uvec2(coord) * uvec2(width, height) / uvec2(pc.width, pc.height);
        float depth = uintBitsToFloat(velk_culling.data[first + at.y * width + at.x]);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(vec3(pow(depth, 0.25)), 1.0));
        return;
    }

    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, coord);
    vec3 color = vec3(0.0);
    if (pc.mode == 15u) {
        if (ids.x != 0u) color = light_color(ids, coord);
    } else if (pc.mode == 14u) {
        // By the kept cut record it is from, white for none.
        if (ids.x != 0u) {
            uint record = velk_ids_record(velk_draw_data.data[velk_ids_draw(ids)], ids);
            color = record == VELK_NO_RECORD ? vec3(1.0) : record_color(record);
        }
    } else if (pc.mode == 13u) {
        // A quad in its colour, darker the more it faces away from overhead, so its shape shows.
        if (ids.x != 0u) {
            VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
            if ((d.flags & VELK_DRAW_QUADS) != 0u) {
                uint quad = velk_ids_slot(d, ids);
                vec3 u = vec3(uintBitsToFloat(velk_culling.data[quad + 3u]), uintBitsToFloat(velk_culling.data[quad + 4u]),
                              uintBitsToFloat(velk_culling.data[quad + 5u]));
                vec3 v = vec3(uintBitsToFloat(velk_culling.data[quad + 6u]), uintBitsToFloat(velk_culling.data[quad + 7u]),
                              uintBitsToFloat(velk_culling.data[quad + 8u]));
                vec3 n = cross(u, v);
                float facing = dot(n, n) > 0.0 ? abs(normalize(n).y) : 1.0;
                color = unpackUnorm4x8(velk_culling.data[quad + 9u]).rgb * (0.4 + 0.6 * facing);
            } else {
                color = vec3(0.25);
            }
        }
    } else if (pc.mode >= 10u) {
        if (ids.x != 0u) {
            uint draw = velk_ids_draw(ids);
            color = id_color(draw * 16381u + velk_ids_slot(velk_draw_data.data[draw], ids) * 8191u) * 0.4;
        }
    } else if (pc.mode >= 7u) {
        if (ids.x != 0u) color = cluster_color(ids);
    } else if (pc.mode >= 5u) {
        if (ids.x != 0u) color = triangle_size_color(ids);
    } else if (ids.x != 0u) {
        uint draw = velk_ids_draw(ids);
        VelkDrawData d = velk_draw_data.data[draw];
        uint id = draw * 16381u;
        // By the instance's slot, which stays the same from frame to frame; a position in the
        // frame's list of what is visible does not.
        if (pc.mode >= 2u) id += velk_ids_slot(d, ids) * 8191u;
        if (pc.mode >= 3u) id += velk_ids_triangle(d, ids) * 131u;
        color = id_color(id);
    }
    if (pc.dim_outside != 0u && ids.x != 0u && outside_chooser(ids)) color *= 0.3;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord, vec4(color, 1.0));
}
)";

// What the points of light's shaders take of the light code, built on the deferred prelude: the
// culling arena (written by the trace), the light grid, whose word is in the light header at
// CELLS_HEADER (pc.cells_header), word 18, and the area lights' axes.
#define VELK_LIGHT_GRID_ONLY_GLSL R"(
layout(set = 1, binding = 16, std430) buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
#define CELLS_HEADER pc.cells_header
)"

// Square area lights' shape, for the points of light's shaders.
#define VELK_LIGHT_AREA_GLSL R"(
// Half of each side of a square area light facing `facing`, `half_size` across from its
// middle: the same axes velk_area_irradiance uses.
void velk_area_axes(vec3 facing, float half_size, out vec3 a, out vec3 b)
{
    vec3 ln = normalize(facing);
    vec3 lt = abs(ln.y) < 0.9 ? normalize(cross(vec3(0.0, 1.0, 0.0), ln)) : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    a = lt * half_size;
    b = cross(ln, lt) * half_size;
}

// The diffuse light reaching a surface at `pos` facing `N` from a square area light with
// nothing in the way, as a factor on the light's colour times intensity: on the same scale as
// a point light's N.L, as velk_area_irradiance's diffuse is used.
float velk_area_diffuse(vec3 middle, vec3 facing, float half_size, vec3 pos, vec3 N)
{
    vec3 a;
    vec3 b;
    velk_area_axes(facing, half_size, a, b);
    vec3 c = middle - pos;
    vec3 p[4] = vec3[4](c - a - b, c + a - b, c + a + b, c - a + b);
    // Only what is above the surface's horizon: clipped there, up to five corners.
    vec3 q[5];
    int m = 0;
    for (int i = 0; i < 4; ++i) {
        vec3 u = p[i];
        vec3 v = p[(i + 1) & 3];
        float du = dot(N, u);
        float dv = dot(N, v);
        if (du >= 0.0 && m < 5) q[m++] = u;
        if ((du >= 0.0) != (dv >= 0.0) && m < 5) q[m++] = mix(u, v, du / (du - dv));
    }
    if (m < 3) return 0.0;
    vec3 f = vec3(0.0);
    for (int i = 0; i < m; ++i) {
        vec3 u = normalize(q[i]);
        vec3 v = normalize(q[i + 1 == m ? 0 : i + 1]);
        vec3 e = cross(u, v);
        float el = length(e);
        if (el > 1e-8) f += e * (acos(clamp(dot(u, v), -1.0, 1.0)) / el);
    }
    return abs(dot(f, N)) * 0.5;
}
)"

// The lights by where they reach, rebuilt each frame (DeferredPath::build_light_grid), in the
// culling arena at the light header's word 18, 0xFFFFFFFF if there is none. Its words: how many
// buckets, a power of two; the size of its cubes, a float; how many lights are listed
// everywhere, and a spare word; those lights; then two words a bucket, where its lights start
// from the grid's first word and how many; then the buckets' lights. A light is listed in the
// cubes its range's box covers, or everywhere. A bucket holds the lights of the cubes that hash
// to it, so it can list lights that do not reach a place in its cube. Follows the declaration
// of velk_light_cell_words and CELLS_HEADER.
#define VELK_LIGHT_GRID_GLSL R"(
uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// How many lights could reach the place `world_at`, from the world's origin, and in `first`
// where its bucket's lights start, for velk_light_grid_at. Every light if there is no grid.
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = velk_light_cell_words.data[grid];
    float size = uintBitsToFloat(velk_light_cell_words.data[grid + 1u]);
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = velk_light_cell_words.data[b + 1u];
    if (count != 0u) first = grid + velk_light_cell_words.data[b];
    return everywhere + count;
}

// The `k`th light that could reach the place velk_light_grid_count gave `first` for.
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    return k < everywhere ? velk_light_cell_words.data[grid + 4u + k] : velk_light_cell_words.data[first + k - everywhere];
}
)"

/// The light code the points of light's passes are built with (the lighting pass and
/// light trace's, VELK_LIGHT_TRACE_MAIN_GLSL): see VELK_LIGHT_GRID_ONLY_GLSL.
[[maybe_unused]] constexpr string_view light_grid_src = VELK_LIGHT_GRID_ONLY_GLSL VELK_LIGHT_AREA_GLSL VELK_LIGHT_GRID_GLSL;

// What lighting a surface takes, for the deferred lighting and the blended surfaces' fragments:
// the scene's lights, the environment, the highlight's terms. Follows VELK_GLOBALS and push
// constants `pc` with env_texture_id and env_params.
#define VELK_SURFACE_LIGHT_GLSL R"(
// Mirrors C++ GpuLight (80 bytes) in ray_tracer.cpp / deferred_lighter.cpp.
struct Light {
    uvec4 flags;           // x = type (0 dir, 1 point, 2 spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz world position (point / spot)
    vec4  direction;       // xyz world forward (dir / spot)
    vec4  color_intensity; // rgb colour, a intensity
    vec4  params;          // x range, y cos(inner), z cos(outer), w light size (dir: angular radius rad; point/spot: world radius)
};

// The place on a square area light that velk_light_sample chose last, which the shadow ray toward
// that light aims at, so that its light and what blocks it are of the same place.
vec3 velk_light_target = vec3(0.0);

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the RT compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

// Equirect env sample with explicit mip LOD. The env texture ships
// with a bilinear-downsampled mip chain as a rough roughness
// prefilter; higher `lod` values read blurrier mips. Returns vec3(0)
// when the view has no environment.
vec3 env_miss_color_lod(vec3 rd, float lod)
{
    if (pc.env_texture_id == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    vec2 params = pc.env_params;
    float c = cos(params.y);
    float s = sin(params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return textureLod(velk_textures[nonuniformEXT(pc.env_texture_id)],
                      vec2(u, v), lod).rgb * params.x;
}

vec3 env_miss_color(vec3 rd) { return env_miss_color_lod(rd, 0.0); }

// GGX normal distribution, Smith geometry, Schlick Fresnel. Same forms
// used by the RT path's velk_fill_standard.
float ggx_d(float NdotH, float a)
{
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265358979323846 * denom * denom, 1e-6);
}

float smith_g1(float NdotX, float a)
{
    float k = (a + 1.0); k = k * k * 0.125; // (a+1)^2 / 8, Schlick-GGX for direct
    return NdotX / max(NdotX * (1.0 - k) + k, 1e-6);
}

float smith_g(float NdotV, float NdotL, float a)
{
    return smith_g1(NdotV, a) * smith_g1(NdotL, a);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0, float F90)
{
    return F0 + (vec3(F90) - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// KHR_materials_specular, with the base colour, metalness and the material's specular colour
// and strength (MaterialEval::specular_color_factor, specular_factor): reflectance at normal
// incidence, and in `F90` at grazing. A dielectric's is the strength times 0.04 tinted by the
// colour (capped at 1), and the strength; a metal's its base colour, and 1.
vec3 velk_specular_f0(vec3 base, float metallic, vec3 specular_color, float specular, out float F90)
{
    specular = clamp(specular, 0.0, 1.0);
    F90 = mix(specular, 1.0, metallic);
    return mix(min(0.04 * max(specular_color, vec3(0.0)), vec3(1.0)) * specular, base, metallic);
}

// Roughness-aware Schlick for environment / IBL specular: scales the grazing
// reflectance (F90) by (1 - roughness), so rough surfaces don't reflect the
// full environment at glancing angles. Without this, rough geometry edges
// (e.g. painted facades) get an over-bright Fresnel rim of the env. A weaker
// specular strength (F90 below 1) weakens the whole lobe in proportion.
vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float F90, float roughness)
{
    vec3 grazing = max(vec3((1.0 - roughness) * F90), F0);
    return F0 + (grazing - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}
)"

// Each light's diffuse light and highlight on a surface, for the deferred lighting and the
// blended surfaces' fragments. Follows VELK_SURFACE_LIGHT_GLSL and velk_area_irradiance, with
// push constants `pc` with light_count, lights_base and ltc_magnitude_id.
#define VELK_SURFACE_LIGHTS_GLSL R"(
// The light the sun pass shades (ICamera::sun_pass), with what of it gets through, in place of what
// the points keep of it: VELK_LIGHT_NONE (all bits) for none.
uint velk_sun_light = 0xFFFFFFFFu;
float velk_sun_through = 1.0;

// Light `i`'s diffuse light and highlight, `through` of it getting through, added to the sums.
// As velk_surface_lights says.
void velk_surface_light(uint i, float through, vec3 world_pos, vec3 N, vec3 V, vec3 F0, float F90, float roughness,
                        float stand_in, float transmission, inout vec3 direct, inout vec3 highlights, inout vec3 area_irr,
                        inout vec3 area_spec)
{
    float NdotV = max(dot(N, V), 0.0);
    float a = roughness * roughness;
    // A stand-in's face, turned to the viewer.
    vec3 face = dot(N, V) < 0.0 ? -N : N;
    {
        Light light = velk_light_at(pc.lights_base + i);
        // Area lights are integrated in closed form, and blocked by what the points say.
        if (light.flags.x == 3u) {
            vec2 es = velk_area_irradiance(light, world_pos, N, V, roughness);
            vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * through;
            // pi converts clamped-cosine units to the same scale as a delta light's N.L, so
            // authored intensities stay comparable.
            float irradiance = max(es.x, 0.0) * (1.0 - transmission);
            if (transmission > 0.0) irradiance += max(velk_area_irradiance(light, world_pos, -N, V, roughness).x, 0.0) * transmission;
            if (irradiance > 0.0) area_irr += radiance * irradiance * 3.14159265;
            if (es.y > 0.0 && pc.ltc_magnitude_id != 0u) {
                // Split-sum: the fitted pair is (lobe energy, Fresnel
                // weight), applied the same way the env specular applies
                // its BRDF LUT, so both specular sources stay on one scale.
                vec2 t2 = velk_texture(pc.ltc_magnitude_id,
                                       vec2(clamp(roughness, 0.0, 1.0),
                                            sqrt(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0)))).xy;
                area_spec += radiance * es.y * (F0 * t2.x + (vec3(F90) - F0) * t2.y)
                             * 3.14159265;
            }
            return;
        }
        vec3 L;
        float atten = 1.0;
        if (light.flags.x == 0u) {
            L = -light.direction.xyz;
        } else {
            vec3 to_light = light.position.xyz - world_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-6);
            float range = max(light.params.x, 1e-6);
            float t = clamp(1.0 - dist / range, 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) {
                float cos_a = dot(-L, light.direction.xyz);
                atten *= smoothstep(light.params.z, light.params.y, cos_a);
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        float diffuse = stand_in >= 0.0 ? mix(velk_ball_response(L, V), max(dot(face, L), 0.0), stand_in)
                                        : mix(NdotL, max(-dot(N, L), 0.0), transmission);
        if ((diffuse <= 0.0 && NdotL <= 0.0) || atten <= 0.0) return;
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten;
        direct += diffuse * radiance * through;
        if (NdotL <= 0.0) return;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);
        float D = ggx_d(NdotH, a);
        float G = smith_g(NdotV, NdotL, roughness);
        vec3 F = fresnel_schlick(VdotH, F0, F90);
        // A light's intensity is the irradiance it gives over pi (diffuse is albedo times it, as the
        // indirect light, a radiance, is), so its highlight takes pi back, as an area light's does.
        highlights += (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6) * NdotL * radiance * through * 3.14159265;
    }
}

// At `world_pos` (from the camera) facing `N`, seen along `V`: through what the points of light
// keep when `points` (the lights `point_lights` and what of each gets through), else every light
// unshadowed; the sun pass's light (velk_sun_light) once, through what it says, whether the
// points keep it or not. Area lights in closed form. A stand-in for what it is too small to draw
// (`stand_in` its flatness, velk_stand_in_flatness; below 0 for a surface as drawn) takes diffuse
// light as a rough ball would, toward its face's as it is flatter, that face turned to the viewer.
// A thin sheet passing `transmission` of its diffuse light takes that share from behind it, the
// rest from in front. Its reflectance is `F0` at normal incidence and `F90` at grazing
// (velk_specular_f0).
void velk_surface_lights(bool points, uint point_lights[VELK_LIGHT_SLOTS], float point_through[VELK_LIGHT_SLOTS],
                         vec3 world_pos, vec3 N, vec3 V, vec3 F0, float F90, float roughness, float stand_in, float transmission,
                         out vec3 direct, out vec3 highlights, out vec3 area_irr, out vec3 area_spec)
{
    direct = vec3(0.0);
    highlights = vec3(0.0);
    area_irr = vec3(0.0);
    area_spec = vec3(0.0);
    uint light_count = points ? VELK_LIGHT_SLOTS : pc.light_count;
    for (uint k = 0u; k < light_count; ++k) {
        if (points && point_lights[k] == VELK_LIGHT_NONE) continue;
        uint i = points ? point_lights[k] : k;
        if (i == velk_sun_light) continue;
        velk_surface_light(i, points ? point_through[k] : 1.0, world_pos, N, V, F0, F90, roughness, stand_in, transmission, direct,
                           highlights, area_irr, area_spec);
    }
    if (velk_sun_light != 0xFFFFFFFFu) {
        velk_surface_light(velk_sun_light, velk_sun_through, world_pos, N, V, F0, F90, roughness, stand_in, transmission, direct,
                           highlights, area_irr, area_spec);
    }
}
)"


// Default compute shader for the deferred lighting pass. Samples the
// G-buffer attachments + light buffer and writes the shaded color to
// the per-view output storage image.
//
// Shader-side responsibilities:
//   - Unlit path: pass albedo through unchanged.
//   - Standard path: evaluate Lambertian diffuse from each analytic
//     light (directional / point / spot) with distance + cone falloff.
//   - Shadow modulation lands in a later slice (B.3.d) alongside the
//     shared `velk_eval_shadow` composer.
[[maybe_unused]] constexpr string_view deferred_compute_prelude_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

// velk_globals (set = 1 slot 2) is declared in velk.glsl. pc.globals_base
// selects this view's record.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

// Material records as raw words (set = 1 slot 4). This shader does not
// evaluate materials, but it composes the same intersect snippets the RT
// compute does, and an intersect may read its shape's record (text reads its
// glyph-data bases from it) at the word base carried on the shape.
layout(set = 1, binding = 4, std430) readonly buffer VelkMaterialWords { uint data[]; } velk_material_words;

// Per-primitive ray geometry (set = 1 slot 11), plus the trees it points at (slots 12 / 13).
// A primitive's runs are allocated once at load and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

// RtShape comes from velk.glsl. View-level globals (inverse_view_projection, the
// view's top tree for rays, present_counter) are read through VELK_GLOBALS.
)" R"(

// scalar layout: tight packing so the per-dispatch CPU struct (whose
// `vec4 cam_pos` sits at offset 0 of the struct) lines up with the
// GPU's view of cam_pos at offset 8 of the push-constant block. Default
// std430 would round vec4 up to a 16-byte alignment and shift every
// subsequent field by 8 bytes vs. what the CPU writes.
layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (4 bytes; view's FrameGlobals index)
    uint spare0;               // 4
    vec4 cam_pos;              // 8         (CPU push starts here)
    uint output_image_id;      // 24
    uint albedo_tex_id;        // 28
    uint normal_tex_id;        // 32
    uint worldpos_tex_id;      // 36
    uint material_tex_id;      // 40
    uint emissive_tex_id;      // 44
    uint width;                // 48
    uint height;               // 52
    uint light_count;          // 56
    uint env_texture_id;       // 60
    uint cells_header;         // 64  word of the light header. See CELLS_HEADER
    uint ltc_matrix_id;        // 68  LTC transform table; 0 = area specular off
    uint lights_base;          // 72  index into velk_lights (set = 1 slot 5)
    uint ltc_magnitude_id;     // 76  LTC energy / Fresnel table
    vec2 env_params;           // 80  x = intensity, y = rotation_rad (inline)
    uint spare6;               // 88
    uint spare1;               // 92
    uint mode;                 // 96  what a pass is to do, for passes that do more than one thing
    uint spare3;               // 100
    uint spare4;               // 104
    uint spare2;               // 108
    uint spare5;               // 112
} pc;
)" VELK_SURFACE_LIGHT_GLSL R"(
#include "velk_intersect.glsl"

// The walk's stack per thread, in shared memory: 48 for 64 threads is 12 KB. The light trace's
// log gives the deepest a frame's rays got.
#define VELK_TRACE_STACK_SIZE 48
#include "velk_trace.glsl"
)"
                                                                      R"(
// Shadow dispatch is composed at pipeline-build time from the snippet
// registry's frame_shadow_techs() set. The composer appends each
// tech's snippet #include plus a velk_eval_shadow switch keyed by the
// registry id. tech_id 0 means "no shadow technique" and returns 1.0.
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal);

// ACES Filmic tone map (Krzysztof Narkowicz fit). Maps 0..inf HDR
// radiance to 0..1 SDR while preserving mid-tone contrast. Stand-in
// until the renderer grows a dedicated HDR target + composite pass with
// per-camera exposure (see design-notes/tone-mapping.md).
vec3 velk_tonemap_aces(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
)";

// Area lights in closed form, for deferred_lighting_main_src. Appended after the prelude,
// whose Light declaration it depends on.
[[maybe_unused]] constexpr string_view deferred_area_light_src = R"(
// Analytic area-light shading. The clamped-cosine integral over the emitter's
// spherical polygon, evaluated in closed form: no samples, no noise, nothing
// for the denoiser to resolve. Validated against Monte Carlo in
// design-notes/spike_analytic/analytic.cpp before being written here.

// Vertex capacity of every polygon handled here: the light's four corners, which
// clipping at the horizon can grow by one.
const int kOccPolyMax = 8;

// One edge's contribution to the vector form factor.
vec3 velk_ff_edge(vec3 a, vec3 b)
{
    float c = clamp(dot(a, b), -1.0, 1.0);
    vec3  e = cross(a, b);
    float el = length(e);
    return (el > 1e-8) ? e * (acos(c) / el) : vec3(0.0);
}

// Clips a polygon against the half-space dot(nrm, p) >= 0, where the plane
// passes through the shading point. Used for both world-relative points and
// directions: the plane goes through the origin, so clipping the chord and
// clipping the great-circle arc give the same direction once normalised.
int velk_clip_half(inout vec3 p[kOccPolyMax], int n, vec3 nrm)
{
    vec3 o[kOccPolyMax];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        vec3 a = p[i];
        vec3 b = p[i + 1 == n ? 0 : i + 1];
        float da = dot(nrm, a);
        float db = dot(nrm, b);
        if (da >= 0.0 && m < kOccPolyMax) { o[m] = a; ++m; }
        if ((da >= 0.0) != (db >= 0.0) && m < kOccPolyMax) {
            o[m] = mix(a, b, da / (da - db));
            ++m;
        }
    }
    for (int i = 0; i < m; ++i) p[i] = o[i];
    return m;
}

// Vector form factor of a spherical polygon given as directions.
vec3 velk_form_factor(vec3 p[kOccPolyMax], int n)
{
    vec3 f = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        f += velk_ff_edge(normalize(p[i]), normalize(p[i + 1 == n ? 0 : i + 1]));
    }
    return f;
}

)"
                                                                      R"(
// Inverse of the LTC transform for (roughness, N.V), from the fitted table.
// The table stores M as (m00, m02, m11, m20) with m22 == 1, and M has no
// coupling between y and the xz plane, so the inverse is closed-form rather
// than a general 3x3 solve. Returned unnormalised: the polygon integral
// normalises its vertices as directions, so any uniform scale on M cancels.
mat3 velk_ltc_minv(float roughness, float ndotv)
{
    // Table axes match the generator: x = roughness, y = sqrt(1 - N.V), which
    // spends resolution near grazing where the lobe changes fastest.
    vec2 uv = vec2(clamp(roughness, 0.0, 1.0), sqrt(clamp(1.0 - ndotv, 0.0, 1.0)));
    vec4 t = velk_texture(pc.ltc_matrix_id, uv);
    float a = t.x, b = t.y, c = t.z, d = t.w;
    float k = a - b * d;
    if (abs(k) < 1e-6) k = (k < 0.0) ? -1e-6 : 1e-6;
    // Columns, GLSL-style: mat3(col0, col1, col2).
    return mat3(vec3(1.0, 0.0, -d),
                vec3(0.0, k / max(c, 1e-6), 0.0),
                vec3(-b, 0.0, a));
}

// Diffuse irradiance (.x) and the LTC specular integral (.y) from a square area light at
// `world_pos`, with nothing in the way. What blocks it comes from the points of light, as for
// any light.
vec2 velk_area_irradiance(Light light, vec3 world_pos, vec3 N, vec3 V, float roughness)
{
    // Shading frame with N as +Z and the view in the +XZ half-plane. The
    // diffuse integral only reads .z so it does not care about the rotation
    // about N, while LTC requires exactly this frame - so one frame serves
    // both and every polygon is built once.
    vec3 nt = V - N * dot(V, N);
    nt = (dot(nt, nt) > 1e-12) ? normalize(nt)
                              : ((abs(N.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), N))
                                                  : normalize(cross(vec3(1.0, 0.0, 0.0), N)));
    vec3 nb = cross(N, nt);

    vec3 ln = normalize(light.direction.xyz);
    vec3 lt = (abs(ln.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), ln))
                                : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    vec3 lb = cross(ln, lt);
    float hs = max(light.params.w, 1e-4);

    vec3 ctr = light.position.xyz - world_pos;
    vec3 ea = lt * hs;
    vec3 eb = lb * hs;

    vec3 lw[4];
    lw[0] = ctr - ea - eb;
    lw[1] = ctr + ea - eb;
    lw[2] = ctr + ea + eb;
    lw[3] = ctr - ea + eb;

    vec3 lp[kOccPolyMax];
    for (int i = 0; i < 4; ++i) {
        lp[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
    }
    // Unclipped corners are kept: the specular polygon is transformed from
    // these and clipped in ITS own space, since the horizon that matters there
    // is the transformed one.
    vec3 lraw[4];
    for (int i = 0; i < 4; ++i) {
        lraw[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
        lp[i] = lraw[i];
    }
    int ln_count = velk_clip_half(lp, 4, vec3(0.0, 0.0, 1.0));
    if (ln_count < 3) return vec2(0.0);

    vec3 f_total = velk_form_factor(lp, ln_count);
    float f_max = abs(f_total.z);

    // Specular: the same emitter through the LTC transform. A zero table id
    // means the fit is unavailable, and the light stays diffuse-only rather
    // than losing its highlight to a black or garbage lookup.
    mat3 minv = mat3(1.0);
    vec3 lps[kOccPolyMax];
    int ls_count = 0;
    float s_max = 0.0;
    if (pc.ltc_matrix_id != 0u) {
        minv = velk_ltc_minv(roughness, clamp(dot(N, V), 0.0, 1.0));
        for (int i = 0; i < 4; ++i) lps[i] = minv * lraw[i];
        ls_count = velk_clip_half(lps, 4, vec3(0.0, 0.0, 1.0));
        if (ls_count >= 3) s_max = abs(velk_form_factor(lps, ls_count).z);
    }

    return vec2(f_max, s_max) / (2.0 * 3.14159265);
}
)";

// Stands in for the analytic area-light path in views that have no area
// lights. The lighting main still calls velk_area_irradiance, but a constant
// zero folds away completely, leaving none of the polygon machinery's register
// footprint behind. Compiling the real thing everywhere cost bistro, which has
// no area lights at all, roughly 3x on deferred.lighting: the code never ran,
// but the registers its arrays reserve are charged to every branch.
[[maybe_unused]] constexpr string_view deferred_area_light_stub_src = R"(
vec2 velk_area_irradiance(Light light, vec3 world_pos, vec3 N, vec3 V, float roughness)
{
    return vec2(0.0);
}
)";

/**
 * The deferred path's driver for blended surfaces' fragments (the transparent pass), composed with
 * a material's eval (compose_eval_fragment): lit as the deferred lighting lights a pixel, by the
 * points of light their record keeps (velk_light_direct, which blended surfaces' fragments list,
 * light_claim_blended_src), else every light unshadowed; indirect light from what their record
 * keeps, else the environment; with the Fresnel boosted opacity of glass. Its head, then the area
 * lights' code (deferred_area_light_src or its stub), then transparent_lit_main_src. Its push
 * constants are the draw's, then the lighting's (TransparentLitPushC).
 */
[[maybe_unused]] constexpr string_view transparent_lit_head_src = R"(
layout(push_constant, scalar) uniform VelkPC {
    uint draw_base;
    uint pool_word;          // the light pool, VELK_LIGHT_NONE for none
    float light_pixels;      // kLightPixels
    uint cells_header;       // word of the light header. See CELLS_HEADER
    uint light_count;
    uint lights_base;
    uint env_texture_id;
    vec2 env_params;         // x = intensity, y = rotation_rad
    uint ltc_matrix_id;
    uint ltc_magnitude_id;
} pc;

#define VELK_GLOBALS velk_globals.data[velk_draw_data.data[pc.draw_base].globals_base]
layout(set = 1, binding = 16, std430) readonly buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
#define CELLS_HEADER pc.cells_header
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_SURFACE_LIGHT_GLSL VELK_LIGHT_GRID_GLSL VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL VELK_LIGHT_READ_GLSL
    VELK_FRAGMENT_TRIANGLE_GLSL;

[[maybe_unused]] constexpr string_view transparent_lit_main_src = VELK_SURFACE_LIGHTS_GLSL R"(
VELK_VARYINGS_IN

VELK_FRAG_OUT(frag_color)
)" VELK_TRACE_FRAGMENT_GLSL R"(

void main()
{
    VelkDrawData d = velk_draw_data.data[pc.draw_base];
    GlobalData globals = velk_globals.data[d.globals_base];
    EvalContext ctx;
    ctx.material_base = d.material_base;
    ctx.texture_id  = d.texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_rel_pos);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_rel_pos + globals.cam_pos.xyz;
    ctx.rel_pos     = v_rel_pos;
    ctx.tangent     = v_world_tangent;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;

    // An unlit material's alpha is its opacity as it stands, its colour as it is.
    float opacity = e.color.a;
    vec3 color = e.color.rgb;
    if (e.lighting_mode == VELK_LIGHTING_STANDARD) {
        vec3 N = normalize(length(e.normal) > 0.0 ? e.normal : v_world_normal);
        // Seen from both sides: the side it shows is the one toward the camera, of its plane as
        // drawn, as the deferred resolve has it.
        bool two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        if (two_sided) {
            vec3 plane = cross(dFdx(v_rel_pos), dFdy(v_rel_pos));
            if (dot(v_world_normal, plane) * dot(-v_rel_pos, plane) < 0.0) N = -N;
        }
        // Only a thin sheet seen from both sides passes light through.
        float transmission = two_sided ? clamp(e.transmission, 0.0, 1.0) : 0.0;
        vec3 V = normalize(-v_rel_pos);
        float NdotV = max(dot(N, V), 0.0);
        float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
        opacity = e.color.a + fresnel * (1.0 - e.color.a);
        float metallic = clamp(e.metallic, 0.0, 1.0);
        float roughness = clamp(e.roughness, 0.04, 1.0);
        float F90;
        vec3 F0 = velk_specular_f0(e.color.rgb, metallic, e.specular_color_factor, e.specular_factor, F90);

        // The lights its points of light keep and what of each gets through; before they are
        // traced, every light, unshadowed.
        uint point_lights[VELK_LIGHT_SLOTS];
        float point_through[VELK_LIGHT_SLOTS];
        bool points = false;
        uint radiance = VELK_LIGHT_NONE;
        uint radiance_dir = VELK_LIGHT_NONE;
        if (pc.pool_word != VELK_LIGHT_NONE) {
            uvec2 ids = uvec2(pc.draw_base + 1u, (v_instance << d.triangle_bits) | uint(gl_PrimitiveID));
            vec3 b;
            float longest;
            velk_fragment_triangle(d, ids, v_rel_pos, b, longest);
            points = velk_light_direct(pc.pool_word, ids, b, v_rel_pos, longest, pc.light_pixels, point_lights,
                                       point_through, radiance, radiance_dir);
        }
        vec3 direct;
        vec3 highlights;
        vec3 area_irr;
        vec3 area_spec;
        velk_surface_lights(points, point_lights, point_through, v_rel_pos, N, V, F0, F90, roughness, -1.0, transmission, direct,
                            highlights, area_irr, area_spec);

        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);
        // What its record keeps of the light other surfaces give it, where the pixels that showed
        // it faced about as it does; else the environment around its normal.
        vec3 indirect = env_miss_color_lod(N, env_max_lod);
        if (radiance != VELK_LIGHT_NONE) {
            uint kept = velk_light_cell_words.data[radiance];
            bool facing = radiance_dir == VELK_LIGHT_NONE
                       || dot(N, unpackSnorm4x8(velk_light_cell_words.data[radiance_dir]).xyz) > 0.5;
            if (kept != VELK_LIGHT_NONE && facing) indirect = velk_rgb9e5(kept);
        }
        // A thin sheet passing light through: that share of its diffuse light from its other side.
        if (transmission > 0.0) indirect = mix(indirect, env_miss_color_lod(-N, env_max_lod), transmission);
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, F90, roughness);
        color = e.color.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;
    }
    frag_color = vec4(color + e.emissive, opacity);
    // The culling's trace, at its pixel: drawn over what is there, with this opacity.
    velk_trace_fragment_note(6u, pc.draw_base, 0u, floatBitsToUint(opacity));
}
)";

[[maybe_unused]] constexpr string_view deferred_lighting_main_src = R"(
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL VELK_LIGHT_READ_GLSL VELK_PROBE_GLSL VELK_SURFACE_LIGHTS_GLSL VELK_SUN_VISIBILITY_GLSL R"(
// The sun pass's terms in pc.mode: its image plus one in the low bits, 0 for none (sun_rays_main_src,
// or sun_soften_main_src softened), and in the top four (VELK_SUN_VIEW) what to show in place of
// the lit image: 0 the lit image, 1 what the lighting takes of the sun, and of the softening 2
// the penumbra's width over 256 pixels, 3 the blocked pixels its search found over 17, 4 the
// pixels its filter took over 17; 5 the surface's N.L toward the sun.
#define VELK_SUN_RAYS_MASK 0x0FFFFFFFu
#define VELK_SUN_SHOW_SHIFT 28u

// The bounce light at a pixel from the screen probes around it (light_probes_main_src, this
// frame's store at pc.spare1): the four nearest, each seen from `n` (velk_probe_eval), by how
// near they are on screen, how much their normal agrees with `n` and how near `pos` is to their
// surface's plane. `along` is the same probes' light seen from `r`, for a rough reflection along
// it, and `match` how well they matched, 0 to 1. False where none of them has a surface like it.
bool velk_probe_indirect(ivec2 coord, vec3 pos, vec3 n, vec3 r, out vec3 light, out vec3 along, out float match)
{
    light = vec3(0.0);
    along = vec3(0.0);
    match = 0.0;
    if (pc.spare1 == 0xFFFFFFFFu) return false;
    const float kProbe = 16.0;
    uint across = (pc.width + 15u) / 16u;
    uint down = (pc.height + 15u) / 16u;
    // A probe sits at the middle pixel of its square, whose centre is 8.5 pixels in.
    vec2 at = (vec2(coord) + 0.5 - (kProbe * 0.5 + 0.5)) / kProbe;
    ivec2 base = ivec2(floor(at));
    vec2 f = at - vec2(base);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 p = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), ivec2(int(across) - 1, int(down) - 1));
        uint w = pc.spare1 + (uint(p.y) * across + uint(p.x)) * VELK_PROBE_WORDS;
        if (uintBitsToFloat(velk_light_cell_words.data[w + VELK_PROBE_FRAMES]) <= 0.0) continue;
        vec3 pn = velk_probe_normal(w);
        vec3 ppos = velk_probe_position(w);
        float bilinear = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
        float facing = pow(max(dot(pn, n), 0.0), 4.0);
        float plane = exp(-abs(dot(pn, pos - ppos)) / (0.02 * length(pos) + 1e-3));
        float weight = bilinear * facing * plane + 1e-6 * bilinear;
        VelkProbeLight probe = velk_probe_read(w);
        light += weight * velk_probe_eval(probe, pn, n);
        along += weight * velk_probe_eval(probe, pn, r);
        total += weight;
    }
    if (total <= 1e-5) return false;
    light /= total;
    along /= total;
    // The bilinear weights add up to one: what is left of them is how much matched.
    match = clamp(total, 0.0, 1.0);
    return true;
}

// Lights each pixel: each light's diffuse light and highlight, shadowed by the points of light
// on the cut (pc.spare3 the pool, pc.spare4 and pc.spare2 the pixel ids' word and width,
// pc.spare5 kLightPixels' bits), unshadowed until they are traced; indirect light from the
// screen probes (velk_probe_indirect), or the environment; reflections from the environment;
// area lights in closed form; emission. Writes the finished image.
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    // While VELK_LIGHT_READ names pixels: the frame the lighting ran in (VELK_POOL_READ_STAMP),
    // which the next frame's sweep copies with this frame's counts.
    if (coord.x == 0 && coord.y == 0 && pc.spare3 != VELK_LIGHT_NONE
        && velk_light_cell_words.data[pc.spare3 + VELK_POOL_READ_PIXELS] != VELK_LIGHT_NONE) {
        velk_light_cell_words.data[pc.spare3 + VELK_POOL_READ_STAMP] = VELK_GLOBALS.present_counter;
    }

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 albedo = velk_texture(pc.albedo_tex_id, uv);
    vec4 normal_and_error = velk_texture(pc.normal_tex_id, uv);
    vec3 world_n = normal_and_error.xyz;
    vec4 position = velk_texture(pc.worldpos_tex_id, uv);
    vec3 world_pos = position.xyz;
    vec4 mat = velk_texture(pc.material_tex_id, uv);

    // No surface: the environment along the view ray. Showing the sun's visibility: clear.
    uint sun_show = pc.mode >> VELK_SUN_SHOW_SHIFT;
    bool show_sun = sun_show != 0u;
    if (dot(world_n, world_n) < 1e-6) {
        if (show_sun) {
            imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(1.0));
            return;
        }
        vec2 ndc = uv * 2.0 - 1.0;
        mat4 inv_vp = VELK_GLOBALS.inverse_view_projection;
        vec4 near_h = inv_vp * vec4(ndc, 1.0, 1.0);
        vec4 far_h = inv_vp * vec4(ndc, 0.0, 1.0);
        vec3 rd = normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(env_miss_color(rd), 1.0));
        return;
    }

    // Added to the lit colour, before tonemapping, so that bloom can pick up bright emitters.
    vec4 emissive_and_transmission = velk_texture(pc.emissive_tex_id, uv);
    vec3 emissive = emissive_and_transmission.rgb;
    // Of its diffuse light, the share a thin sheet passes to its other side (MaterialEval::transmission).
    float transmission = emissive_and_transmission.a;

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    // A stand-in for what is too small to draw (velk_stand_in_flatness) reflects as roughly as
    // what it stands for faces every way: not as a smooth face seen edge on.
    float stand_in = velk_stand_in_unpack(mat.a);
    if (stand_in >= 0.0) roughness = mix(1.0, roughness, stand_in);

    vec3 color = albedo.rgb;
    if (show_sun && lighting_mode == 0u) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(1.0));
        return;
    }
    if (lighting_mode != 0u) {
        vec3 N = normalize(world_n);
        vec3 V = normalize(-world_pos);
        float NdotV = max(dot(N, V), 0.0);
        // A dielectric's reflectance (velk_specular_f0), its square root at normal incidence and
        // its grazing one (the resolve's RESOLVE_SPECULAR, here pc.spare6); a metal's its colour.
        vec4 specular = velk_texture(pc.spare6, uv);
        vec3 F0 = mix(specular.rgb * specular.rgb, albedo.rgb, metallic);
        float F90 = mix(specular.a, 1.0, metallic);

        // The lights its points of light keep and what of each gets through; before its points
        // are traced, every light, unshadowed.
        uint point_lights[VELK_LIGHT_SLOTS];
        float point_through[VELK_LIGHT_SLOTS];
        bool points = false;
        uint radiance = VELK_LIGHT_NONE;
        uint radiance_dir = VELK_LIGHT_NONE;
#ifdef VELK_LIGHT_CODES
        g_light_codes_first = VELK_LIGHT_NONE;
        g_light_pending_slots = 0u;
#endif
        if (pc.spare3 != VELK_LIGHT_NONE) {
            points = velk_light_direct(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord),
                                       velk_pixel_weights(pc.spare4, pc.spare2, coord), world_pos, position.w,
                                       uintBitsToFloat(pc.spare5), point_lights, point_through, radiance, radiance_dir);
        }
        // Counted, for the log: a lit pixel whose light is not traced yet.
        if (!points && pc.spare3 != VELK_LIGHT_NONE) atomicAdd(velk_light_cell_words.data[pc.spare3 + VELK_POOL_UNTRACED_PIXELS], 1u);
        // The sun pass: what of the sun gets through at this pixel, in place of what its points keep.
        uint sun_rays = pc.mode & VELK_SUN_RAYS_MASK;
        if (sun_rays != 0u) {
            velk_sun_light = velk_sun_index();
            velk_sun_through = texelFetch(velk_textures[nonuniformEXT(sun_rays - 1u)], coord, 0).x;
        }
#ifdef VELK_LIGHT_CODES
        // For the log, while VELK_LIGHT_READ is set: this pixel's lights that bring it light and
        // whose points have not traced them yet, or a change made stale; before its points are
        // traced, every light that brings it light. Added to the pool's PENDING.
        if (pc.spare3 != VELK_LIGHT_NONE) {
            uint pending = 0u;
            if (points) {
                for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
                    uint i = point_lights[k];
                    if (i == VELK_LIGHT_NONE || i >= pc.light_count || i == velk_sun_light) continue;
                    if (velk_light_strength(velk_light_at(pc.lights_base + i), world_pos) <= 0.0) continue;
                    uint s = velk_light_codes_slot(i);
                    if (s < VELK_LIGHT_SLOTS && (g_light_pending_slots & (1u << s)) != 0u) ++pending;
                }
            } else {
                uint count = min(pc.light_count, 2048u);
                for (uint i = 0u; i < count; ++i) {
                    if (i == velk_sun_light) continue;
                    if (velk_light_strength(velk_light_at(pc.lights_base + i), world_pos) > 0.0) ++pending;
                }
            }
            if (pending != 0u) atomicAdd(velk_light_cell_words.data[pc.spare3 + VELK_POOL_PENDING], pending);
        }
#endif
)" R"(
        // Showing what the lighting takes of the sun, from the sun pass or its points; magenta where
        // its points are not traced yet.
        if (show_sun) {
            uint sun = velk_sun_index();
            vec3 shown = vec3(1.0, 0.0, 1.0);
            if (sun_rays != 0u || sun == VELK_LIGHT_NONE) {
                shown = vec3(velk_sun_through);
            } else if (points) {
                shown = vec3(0.0);
                for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
                    if (point_lights[k] == sun) shown = vec3(point_through[k]);
                }
            }
            // The softening's own terms (sun_soften_main_src's z and w).
            if (sun_show >= 2u && sun_show <= 4u && sun_rays != 0u) {
                vec4 terms = texelFetch(velk_textures[nonuniformEXT(sun_rays - 1u)], coord, 0);
                float found = floor(terms.w / 32.0);
                shown = vec3(sun_show == 2u ? terms.z / 256.0 : sun_show == 3u ? found / 17.0 : (terms.w - found * 32.0) / 17.0);
            }
            // The surface's N.L toward the sun.
            if (sun_show == 5u && sun != VELK_LIGHT_NONE) shown = vec3(max(dot(normalize(world_n), -velk_light_at(pc.lights_base + sun).direction.xyz), 0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(shown, 1.0));
            return;
        }
        vec3 direct;
        vec3 highlights;
        vec3 area_irr;
        vec3 area_spec;
        velk_surface_lights(points, point_lights, point_through, world_pos, N, V, F0, F90, roughness, stand_in, transmission, direct,
                            highlights, area_irr, area_spec);

        // Env lighting: single-sample approximation. Diffuse reads along N with a deep LOD
        // so it reads roughly the irradiance average; specular reads the mirror reflection
        // with LOD scaled by roughness as a cheap GGX-prefilter stand-in.
        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_diffuse = env_miss_color_lod(N, env_max_lod);
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);

        // Light from other surfaces and the sky: the screen probes around it, or the environment
        // where none has a surface like it yet. The same probes seen along the reflection stand in
        // for a rough reflection, shadowed as the environment is not, as far as they matched the
        // surface.
        vec3 indirect;
        vec3 probe_specular;
        float match;
        bool probed = velk_probe_indirect(coord, world_pos, N, reflect(-V, N), indirect, probe_specular, match);
        if (probed) env_specular = mix(env_specular, probe_specular, clamp(match * 4.0, 0.0, 1.0));
        // Traced reflections (reflect_resolve_src, pc.spare0) where the surface is smooth enough
        // for them, fading into the rough reflection toward the roughness they stop at.
        if (pc.spare0 != VELK_LIGHT_NONE) {
            vec4 traced = texelFetch(velk_textures[nonuniformEXT(pc.spare0)], coord, 0);
            if (traced.a > 0.0) env_specular = mix(env_specular, traced.rgb, 1.0 - smoothstep(0.21, 0.3, roughness));
        }
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, F90, roughness);

        if (!probed) {
            indirect = env_diffuse;
        } else if (radiance != VELK_LIGHT_NONE
                   && ((uint(coord.x) & 3u) | ((uint(coord.y) & 3u) << 2u)) == (VELK_GLOBALS.present_counter * 7u & 15u)) {
            // A pixel in 16 each frame, a 4 by 4 pattern that moves every frame, keeps its record's
            // indirect light for rays that hit it off screen (velk_probe_hit_indirect): the first
            // as it is, then a little of each; and the way it faces, likewise, not made of unit
            // length again, so that a record seen facing many ways keeps a short one. Pixels of one
            // record race; any of them will do.
            uint kept = velk_light_cell_words.data[radiance];
            bool first = kept == VELK_LIGHT_NONE;
            velk_light_cell_words.data[radiance] = velk_pack_rgb9e5(first ? indirect : mix(velk_rgb9e5(kept), indirect, 0.125));
            if (radiance_dir != VELK_LIGHT_NONE) {
                vec3 way = first ? N : mix(unpackSnorm4x8(velk_light_cell_words.data[radiance_dir]).xyz, N, 0.125);
                velk_light_cell_words.data[radiance_dir] = packSnorm4x8(vec4(way, 0.0));
            }
        }

        // A thin sheet passing light through: that share of its diffuse light from its other side.
        if (transmission > 0.0) {
            vec3 back;
            vec3 back_specular;
            float back_match;
            if (!velk_probe_indirect(coord, world_pos, -N, reflect(-V, -N), back, back_specular, back_match)) {
                back = env_miss_color_lod(-N, env_max_lod);
            }
            indirect = mix(indirect, back, transmission);
        }

        color = albedo.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;

        // At VELK_LIGHT_READ's pixels, its terms for the log (VELK_POOL_READ_TERMS).
        if (pc.spare3 != VELK_LIGHT_NONE && velk_light_cell_words.data[pc.spare3 + VELK_POOL_READ_PIXELS] != VELK_LIGHT_NONE) {
            uint here = uint(coord.x) | (uint(coord.y) << 16u);
            for (uint k = 0u; k < 4u; ++k) {
                if (velk_light_cell_words.data[pc.spare3 + VELK_POOL_READ_PIXELS + k] != here) continue;
                uint at = pc.spare3 + VELK_POOL_READ_TERMS + k * VELK_LIGHT_READ_WORDS;
                vec3 terms[7] = vec3[7](albedo.rgb, direct, indirect, env_diffuse, highlights + F_env * env_specular + area_spec,
                                        area_irr, color);
                for (uint t = 0u; t < 7u; ++t) {
                    for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + t * 3u + c] = floatBitsToUint(terms[t][c]);
                }
                velk_light_cell_words.data[at + VELK_LIGHT_READ_METALLIC] = floatBitsToUint(metallic);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FLAGS] = (points ? 1u : 0u) | (probed ? 2u : 0u)
                    | (VELK_GLOBALS.present_counter << 8u);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_MATCH] = floatBitsToUint(match);
                uint explain[VELK_LIGHT_EXPLAIN_WORDS];
                velk_light_explain(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord), velk_pixel_weights(pc.spare4, pc.spare2, coord),
                                   world_pos, position.w, explain);
                for (uint e = 0u; e < VELK_LIGHT_EXPLAIN_WORDS; ++e) {
                    velk_light_cell_words.data[at + VELK_LIGHT_READ_EXPLAIN + e] = explain[e];
                }
                uint first_light = points ? point_lights[0] : VELK_LIGHT_NONE;
                float n_dot_l = 0.0;
                if (first_light != VELK_LIGHT_NONE) n_dot_l = dot(N, -normalize(velk_light_at(pc.lights_base + first_light).direction.xyz));
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT] = first_light;
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 1u] = floatBitsToUint(points ? point_through[0] : -1.0);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 2u] = floatBitsToUint(n_dot_l);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 3u] = velk_pixel_ids(pc.spare4, pc.spare2, coord).x;
                for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + VELK_LIGHT_READ_HIGHLIGHTS + c] = floatBitsToUint(highlights[c]);
                for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + VELK_LIGHT_READ_NORMAL + c] = floatBitsToUint(N[c]);
            }
        }
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(color + emissive, albedo.a));
}
)";

// The sun pass (ICamera::sun_pass): a ray a pixel to the middle of the sun (velk_sun_visibility), from
// the G-buffer's surface, for the lighting to take in place of what the sun's points keep. Writes
// to pc.output_image_id the visibility in x and in y how far away what was in the way is: 0 where
// nothing was, -1 where the surface faces away from the sun. Clear where there is no surface or
// no sun. Built on the deferred prelude.
[[maybe_unused]] constexpr string_view sun_rays_main_src = VELK_SUN_VISIBILITY_GLSL R"(
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec3 n = velk_texture(pc.normal_tex_id, uv).xyz;
    uint sun = velk_sun_index();
    vec4 result = vec4(1.0, 0.0, 0.0, 1.0);
    if (dot(n, n) >= 1e-6 && sun != 0xFFFFFFFFu) {
        vec3 pos = velk_texture(pc.worldpos_tex_id, uv).xyz;
        // A thin sheet passing light through is lit from the side the sun is on.
        bool two_sided = velk_texture(pc.emissive_tex_id, uv).a > 0.0;
        bool stand_in = velk_stand_in_unpack(velk_texture(pc.material_tex_id, uv).a) >= 0.0;
        float blocker_t;
        bool facing_away;
        result.x = velk_sun_visibility(pos, normalize(n), two_sided, stand_in, -velk_light_at(pc.lights_base + sun).direction.xyz,
                                       blocker_t, facing_away);
        result.y = facing_away ? -1.0 : blocker_t;
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, result);
}
)";

// The sun pass as sun_rays_main_src, which also adds the rays that walked to the pool's SUN_RAYS,
// the pool's word in pc.spare3 (VELK_LIGHT_NONE for none), for the log: used only while
// VELK_LIGHT_READ is set, so that the sun pass otherwise carries no count. Built on the deferred
// prelude with the light grid.
[[maybe_unused]] constexpr string_view sun_rays_count_main_src = R"(
#define VELK_LIGHT_WORDS velk_light_cell_words
)" VELK_LIGHT_POOL_HEADER_GLSL VELK_SUN_VISIBILITY_GLSL R"(
shared uint s_sun_rays;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (gl_LocalInvocationIndex == 0u) s_sun_rays = 0u;
    barrier();
    if (coord.x < int(pc.width) && coord.y < int(pc.height)) {
        vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
        vec3 n = velk_texture(pc.normal_tex_id, uv).xyz;
        uint sun = velk_sun_index();
        vec4 result = vec4(1.0, 0.0, 0.0, 1.0);
        if (dot(n, n) >= 1e-6 && sun != 0xFFFFFFFFu) {
            vec3 pos = velk_texture(pc.worldpos_tex_id, uv).xyz;
            // A thin sheet passing light through is lit from the side the sun is on.
            bool two_sided = velk_texture(pc.emissive_tex_id, uv).a > 0.0;
            bool stand_in = velk_stand_in_unpack(velk_texture(pc.material_tex_id, uv).a) >= 0.0;
            float blocker_t;
            bool facing_away;
            result.x = velk_sun_visibility(pos, normalize(n), two_sided, stand_in,
                                           -velk_light_at(pc.lights_base + sun).direction.xyz, blocker_t, facing_away);
            result.y = facing_away ? -1.0 : blocker_t;
            if (!facing_away) atomicAdd(s_sun_rays, 1u);
        }
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, result);
    }
    barrier();
    if (gl_LocalInvocationIndex == 0u && s_sun_rays != 0u && pc.spare3 != 0xFFFFFFFFu) {
        atomicAdd(velk_light_cell_words.data[pc.spare3 + VELK_POOL_SUN_RAYS], s_sun_rays);
    }
}
)";

// The sun pass's softening (SunPass::Soft): the hard visibility of the sun rays (pc.spare0,
// sun_rays_main_src) spread over the penumbra the sun's disk would give, as wide as the mean
// distance to what is in the way says, in the same frame. First that distance, from the blocked
// pixels around within kSunSearchPixels, the pixel's own too; then the visibility of the pixels
// within the penumbra. Both take only pixels on a surface like the pixel's (its plane, the way
// it faces) and never one facing away from the sun, and keep the hard visibility where they find
// nothing. kSunTaps a disk each, turned by a fixed amount a pixel, so that a still view stays
// still. The penumbra is capped at kSunPenumbraPixels. Writes the visibility to x of
// pc.output_image_id, its y as the sun rays', and for VELK_SUN_VIEW the penumbra's width in pixels
// before the cap in z, 0 where there is none, and in w the blocked pixels the search found times
// 32 plus the pixels the filter took. Built on the deferred prelude.
[[maybe_unused]] constexpr string_view sun_soften_main_src = VELK_SUN_VISIBILITY_GLSL R"(
const uint kSunTaps = 16u;
const float kSunSearchPixels = 32.0;
const float kSunPenumbraPixels = 64.0;

// Whether the pixel at `q` is on a surface like the one at `p` facing `n`.
bool velk_sun_alike(ivec2 q, vec3 p, vec3 n, out vec3 pq)
{
    vec3 nq = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], q, 0).xyz;
    pq = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], q, 0).xyz;
    if (dot(nq, nq) < 1e-6) return false;
    if (dot(normalize(nq), n) < 0.7) return false;
    return abs(dot(n, pq - p)) < 0.05 * length(pq - p) + 0.002 * length(p);
}

// Tap `k` of kSunTaps of a disk `radius` pixels wide around `c`, turned by `turn`.
ivec2 velk_sun_tap(ivec2 c, uint k, float radius, float turn)
{
    float r = sqrt((float(k) + 0.5) / float(kSunTaps)) * radius;
    float a = float(k) * 2.39996323 + turn;
    return c + ivec2(round(vec2(cos(a), sin(a)) * r));
}

// Where in the screen, in pixels, the point `u` off `p` perpendicular to the light `L` lands when
// carried along the light onto the plane through `p` facing `n` (`lean` its N.L, away from zero):
// false behind the camera.
bool velk_sun_land(vec3 p, vec3 n, vec3 L, float lean, vec3 u, mat4 vp, out vec2 px)
{
    vec4 clip = vp * vec4(p + u - L * (dot(n, u) / lean), 1.0);
    px = vec2(0.0);
    if (!(clip.w > 1e-6)) return false;
    px = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    return true;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    vec4 hard = texelFetch(velk_textures[nonuniformEXT(pc.spare0)], coord, 0);
    vec3 n = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], coord, 0).xyz;
    uint sun = velk_sun_index();
    vec3 p = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], coord, 0).xyz;
    // Pixels a world unit spans at this depth: from the projection (view_projection works on
    // positions from the camera, as the G-buffer's are), its w 1 for an orthographic one.
    mat4 vp = VELK_GLOBALS.view_projection;
    float clip_w = (vp * vec4(p, 1.0)).w;
    if (hard.y < 0.0 || dot(n, n) < 1e-6 || sun == 0xFFFFFFFFu || !(clip_w > 1e-6)) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(hard.xy, 0.0, 0.0));
        return;
    }
    n = normalize(n);
    float per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * VELK_GLOBALS.viewport.y * 0.5 / clip_w;
    float spread = tan(max(velk_light_at(pc.lights_base + sun).params.w, 0.0));
    uint h = (uint(coord.x) * 0x8da6b343u) ^ (uint(coord.y) * 0xd8163841u);
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    float turn = float(h & 0xFFFFu) / 65536.0 * 6.2831853;

    // The mean distance to what is in the way, from the blocked pixels around and this one.
    float sum = hard.y > 0.0 ? hard.y : 0.0;
    float count = hard.y > 0.0 ? 1.0 : 0.0;
    for (uint k = 0u; k < kSunTaps; ++k) {
        ivec2 q = velk_sun_tap(coord, k, kSunSearchPixels, turn);
        if (q.x < 0 || q.y < 0 || q.x >= int(pc.width) || q.y >= int(pc.height)) continue;
        vec4 hq = texelFetch(velk_textures[nonuniformEXT(pc.spare0)], q, 0);
        if (hq.y <= 0.0) continue;
        vec3 pq;
        if (!velk_sun_alike(q, p, n, pq)) continue;
        sum += hq.y;
        count += 1.0;
    }
    // The penumbra on the receiver: the sun's disc as wide as the mean distance to what is in the
    // way says, perpendicular to the light, carried along the light onto the pixel's plane (an
    // ellipse, the longer the more the plane leans from the light) and projected to the screen.
    // A stand-in has no plane, and a plane nearly along the light would stretch it without end: a
    // disc in the screen for both. Scaled down as a whole where it reaches past kSunPenumbraPixels.
    float world_radius = count > 0.0 ? sum / count * spread : 0.0;
    vec3 L = -velk_light_at(pc.lights_base + sun).direction.xyz;
    float lean = dot(n, L);
    bool stand_in = velk_stand_in_unpack(texelFetch(velk_textures[nonuniformEXT(pc.material_tex_id)], coord, 0).a) >= 0.0
                 || abs(lean) < 0.15;
    vec3 axis_a = normalize(abs(L.y) < 0.9 ? cross(L, vec3(0.0, 1.0, 0.0)) : cross(L, vec3(1.0, 0.0, 0.0)));
    vec3 axis_b = cross(L, axis_a);
    vec2 centre = vec2(coord) + 0.5;
    // Each tap's offset in the screen, NaN-free: a tap landing behind the camera has none, and is
    // left out. The widest of them is the penumbra's width; all are scaled down together when it
    // passes the cap.
    vec2 offsets[kSunTaps];
    uint landed = 0u;
    float wide = 0.0;
    for (uint k = 0u; k < kSunTaps; ++k) {
        float a = float(k) * 2.39996323 + turn + 1.3;
        float r = sqrt((float(k) + 0.5) / float(kSunTaps));
        offsets[k] = vec2(0.0);
        if (stand_in) {
            offsets[k] = vec2(cos(a), sin(a)) * r * world_radius * per_unit;
        } else {
            vec2 px;
            if (!velk_sun_land(p, n, L, lean, (axis_a * cos(a) + axis_b * sin(a)) * r * world_radius, vp, px)) continue;
            offsets[k] = px - centre;
        }
        landed |= 1u << k;
        wide = max(wide, length(offsets[k]));
    }
    float scale = wide > kSunPenumbraPixels ? kSunPenumbraPixels / wide : 1.0;
    if (count == 0.0 || wide * scale < 0.5) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(hard.xy, wide, count * 32.0));
        return;
    }

    // The visibility over the penumbra, of the pixels on a surface like this one.
    float seen = hard.x;
    float weight = 1.0;
    for (uint k = 0u; k < kSunTaps; ++k) {
        if ((landed & (1u << k)) == 0u) continue;
        ivec2 q = ivec2(floor(centre + offsets[k] * scale));
        if (q.x < 0 || q.y < 0 || q.x >= int(pc.width) || q.y >= int(pc.height)) continue;
        vec4 hq = texelFetch(velk_textures[nonuniformEXT(pc.spare0)], q, 0);
        if (hq.y < 0.0) continue;
        vec3 pq;
        if (!velk_sun_alike(q, p, n, pq)) continue;
        seen += hq.x;
        weight += 1.0;
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(seen / weight, hard.y, wide, count * 32.0 + weight));
}
)";

// Compute ray tracer prelude. Fixed header that precedes all material
// snippets. Declares extensions, storage-image binding, shape struct,
// push constants, intersect_rect, and the full stochastic-RT toolkit
// (RNG, GGX sampling, trace_ray). velk_resolve_fill is forward-declared
// so trace_ray can call it; the composer generates its definition after
// all material snippets and before main.
[[maybe_unused]] constexpr string_view rt_compute_prelude_src = R"(
#version 450
#define VELK_COMPUTE 1
#include "velk.glsl"
#include "velk-ui.glsl"

// A work group of VELK_RT_GROUP_THREADS threads in a row where the composer defines it
// (compose_rt_trace), else 8 by 8.
#ifdef VELK_RT_GROUP_THREADS
layout(local_size_x = VELK_RT_GROUP_THREADS, local_size_y = 1, local_size_z = 1) in;
#else
#define VELK_RT_GROUP_THREADS 64
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
// Float storage, for targets carrying HDR rather than display values.
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];

// The per-view frame state (inv-VP, cam, the view's top tree for rays, present_counter)
// comes from the bound FrameGlobals record, indexed by pc.globals_base. See VELK_GLOBALS
// below.

// Scene light. Mirrors the C++ GpuLight struct (80 bytes).
struct Light {
    uvec4 flags;           // x = type (0=dir, 1=point, 2=spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz = world position (point / spot)
    vec4  direction;       // xyz = world forward axis (directional / spot)
    vec4  color_intensity; // rgb = colour, a = intensity multiplier
    vec4  params;          // x = range, y = cos(inner), z = cos(outer), w = light size (dir: angular radius rad; point/spot: world radius)
};

// The place on a square area light that velk_light_sample chose last, which the shadow ray toward
// that light aims at, so that its light and what blocks it are of the same place.
vec3 velk_light_target = vec3(0.0);

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the deferred-lighting compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// Per-primitive ray geometry (set = 1 slot 11), plus the trees it points at (slots 12 / 13),
// the same buffers the deferred prelude reads. A primitive's runs are allocated once at load
// and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

// Material records as raw words (set = 1 slot 4). The raster path binds this
// same slot as a typed block, which one pipeline can do because it compiles
// for a single material type; this shader composes many, so it reads the
// bytes and each material's generated velk_unpack_<T> rebuilds its own struct
// from them. Materials whose layout cannot be derived stay on device
// addresses and never touch this buffer.
layout(set = 1, binding = 4, std430) readonly buffer VelkMaterialWords { uint data[]; } velk_material_words;

// This shader composes many materials, so slot 4 is bound above as raw words
// and must not be re-declared as a typed block: VELK_MATERIAL(T) becomes a
// no-op, and the composer replaces each material's VELK_MATERIAL(T) line with
// its generated velk_unpack_<T>. The load pastes the type name onto that
// function, so every snippet's `VELK_LOAD_MATERIAL(T, ctx)` resolves to its
// own reader. A material whose record cannot be modelled gets no reader and
// fails to compile here, rather than silently reading the wrong bytes.
#undef VELK_MATERIAL
#undef VELK_LOAD_MATERIAL
#define VELK_MATERIAL(T)
#define VELK_LOAD_MATERIAL(T, ctx) velk_unpack_##T((ctx).material_base)

// RT root: per-dispatch state pushed inline as a push constant (no device
// address anywhere). scalar layout matches the C++ `RtRoot` struct.
//
// The camera matrices, BVH root/counts/bases and present_counter live in
// the bound FrameGlobals record (indexed by globals_base), not here. Only
// the RT-specific per-dispatch state remains, all indices / inline data.
// Baked into the cached secondary at record time like the deferred PC;
// globals_base rotates with the globals ring but the cached pass re-records
// on view change, and static globals match across ring slots.
layout(push_constant, scalar) uniform PC {
    uint shapes_base;           // the main's own: the reference path's restart, unused otherwise
    uint globals_base;          // FrameGlobals index (set = 1 slot 2)
    uint light_count;
    uint lights_base;           // index into velk_lights (set = 1 slot 5)
    vec2 env_params;            // x = intensity, y = rotation_rad (inline)
    uvec4 extras;               // the main's own: for the RT path x=image_index, y=width, z=height
    uvec4 env;                  // x=env_material_id, y=env_texture_id, zw=_
    uvec4 more;                 // the main's own, past what extras holds; unset where unused
} pc;
// This view's FrameGlobals, read by index from the bound globals buffer
// (set = 1 slot 2, declared in velk.glsl) instead of a device address.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

// ===== Core ray / hit / fill-context types =====
#include "velk_intersect.glsl"

// EvalContext + MaterialEval shared between raster and RT come from
// velk-ui.glsl (included above).

// Result of evaluating a material at a hit. GLSL forbids recursion, so
// materials cannot call trace_ray; instead they return the local emission
// they produce at this hit plus (optionally) a bounced ray for the
// iterative trace loop in main() to continue with.
//
//   emission    rgb = color contributed at this hit (already weighted for
//               alpha at primary); a = opacity for primary compositing.
//   throughput  multiplier applied to the next bounce's contribution.
//   next_dir    world-space direction of the next ray (ignored if
//               terminate == true).
//   terminate   true = no further bounces (flat UI material, text, env).
//   sample_count_hint  material's preferred number of bounce samples at
//               this hit (e.g. 1 for a mirror, higher for rough GGX).
//               Tracer caps against a global per-bounce budget.
struct BrdfSample {
    vec4 emission;
    vec3 throughput;
    vec3 next_dir;
    bool terminate;
    uint sample_count_hint;
};

// The walk's stack per thread, in shared memory: 48 for 64 threads is 12 KB, for 32 threads 6 KB,
// as the light trace's, since partitions' trees may be 16 levels deeper than balanced. The light
// trace's log gives the deepest the probes' rays got.
#define VELK_TRACE_STACK_SIZE 48
#include "velk_trace.glsl"

)" R"(
// ===== RNG: PCG-hash seeded by (pixel, frame). =====
uint g_rng_state;

uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

void rng_init(uvec2 coord, uint frame) {
    g_rng_state = pcg_hash(coord.x ^ pcg_hash(coord.y ^ pcg_hash(frame + 1u)));
}

uint rng_next_uint() {
    g_rng_state = pcg_hash(g_rng_state);
    return g_rng_state;
}

float rng_next_float() {
    // 24-bit mantissa precision; top 8 bits unused.
    return float(rng_next_uint() >> 8u) * (1.0 / 16777216.0);
}

vec2 rng_next_vec2() { return vec2(rng_next_float(), rng_next_float()); }

// ===== GGX microfacet sampling (isotropic). =====
// Returns a unit half-vector H in world space distributed as GGX(roughness).
vec3 ggx_sample_half(vec3 N, float roughness, vec2 xi) {
    float a = roughness * roughness;
    float phi = 2.0 * 3.14159265 * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

    vec3 H_local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // Build a tangent basis around N. Uses a standard "pick non-parallel up"
    // trick to avoid degenerate cross products.
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H_local.x + B * H_local.y + N * H_local.z);
}

// Sample a reflection direction: pick a microfacet H, reflect V around it.
vec3 ggx_sample_reflect(vec3 V, vec3 N, float roughness, vec2 xi) {
    vec3 H = ggx_sample_half(N, roughness, xi);
    return reflect(-V, H);
}

// Forward declaration. The composer emits the actual definition after
// material #includes. Materials are pure (no recursion into trace_ray).
BrdfSample velk_resolve_fill(uint mid, EvalContext ctx);

// Forward declaration for the shadow-technique dispatch. Emitted by
// the composer after shadow-technique #includes. Materials that want
// to attenuate their direct-lighting contribution by occlusion call
// this with the light's shadow_tech_id; tech_id 0 returns 1.0 (fully
// lit, no shadow technique attached to that light).
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal);

// Sample the environment along a direction (or return black if the
// camera has no env). Inlined here rather than going through the env
// material's velk_fill_env, because materials may need to call this
// (e.g. StandardMaterial's diffuse term) and calling velk_resolve_fill
// from inside a fill would re-introduce the recursion GLSL forbids.
// Mirrors EnvMaterial's equirect sampling; env params (x = intensity,
// y = rotation_rad) ride the push constant inline (pc.env_params).
vec3 env_miss_color(vec3 rd) {
    if (pc.env.x == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return velk_texture(pc.env.y, vec2(u, v)).rgb * pc.env_params.x;
}
)";

// Shared PBR shading helper. Converts a MaterialEval produced by a
// material's velk_eval_<name> into a BrdfSample. Every Lit material
// routes through this instead of open-coding its own PBR body.
//
// Placement: composer emits this string between `velk_eval_shadow` and
// `velk_resolve_fill` so it can call the former and be called by the
// latter. Depends on pc.lights, pc.light_count, env_miss_color,
// velk_eval_shadow, ggx_sample_half, rng_next_vec2 — all in scope at
// that point.
[[maybe_unused]] constexpr string_view rt_pbr_shade_src = R"(
// Set to a light index to shade against that light alone (see the loop bounds
// below); -1 shades against all of them. Per-invocation, like any GLSL global.
int velk_single_light = -1;
// The reciprocal of the probability with which that light was chosen. A caller
// picking uniformly sets the light count; one importance-sampling sets
// total_weight / chosen_weight. Ignored unless velk_single_light is set.
float velk_single_light_weight = 1.0;

// Direction to a light and its distance / spot attenuation, with no shadow
// term. Shared rather than open-coded twice, so a caller weighting lights by
// their unshadowed contribution uses exactly the maths the shading loop will
// then apply.
void velk_light_sample(Light light, vec3 pos, out vec3 L, out float atten)
{
    atten = 1.0;
    if (light.flags.x == 0u) {
        L = -light.direction.xyz;
    } else if (light.flags.x == 3u) {
        // A square area light: one place on it, chosen evenly (velk_light_target, which its shadow
        // ray aims at), weighted so that over many its N.L times this is what the deferred
        // lighting's closed form gives (velk_area_irradiance's diffuse, times pi): the square's
        // area, its cosine toward `pos` on either face, over the distance squared.
        vec3 ln = normalize(light.direction.xyz);
        vec3 lt = (abs(ln.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), ln))
                                    : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
        vec3 lb = cross(ln, lt);
        float hs = max(light.params.w, 1e-4);
        vec2 u = rng_next_vec2() * 2.0 - 1.0;
        velk_light_target = light.position.xyz + (lt * u.x + lb * u.y) * hs;
        vec3 to_light = velk_light_target - pos;
        float dist2 = max(dot(to_light, to_light), 1e-8);
        L = to_light * inversesqrt(dist2);
        atten = 4.0 * hs * hs * abs(dot(ln, L)) / dist2;
    } else {
        vec3 to_light = light.position.xyz - pos;
        float dist = length(to_light);
        L = to_light / max(dist, 1e-6);
        float range = max(light.params.x, 1e-6);
        float t = clamp(1.0 - dist / range, 0.0, 1.0);
        atten = t * t;
        if (light.flags.x == 2u) {
            float cos_a = dot(-L, light.direction.xyz);
            atten *= smoothstep(light.params.z, light.params.y, cos_a);
        }
    }
}

// What the last shaded surface passes on of the light arriving from around it. Zero for
// materials that do not go through velk_pbr_shade. And of that, the share it passes through
// from its other side (MaterialEval::transmission).
vec3 velk_hit_diffuse = vec3(0.0);
float velk_hit_transmission = 0.0;
// Whether the surface being shaded is seen from both sides (VELK_DRAW_DOUBLE_SIDED), set by
// velk_hit_context: only such a thin sheet passes light through.
bool velk_hit_two_sided = false;
// Whether the lights' highlights count where this path is: from the camera and along mirror
// reflections, but not once it has bounced off a surface diffusely, as the deferred lighting's
// bounce light is diffuse only (and those highlights' light, the lights seen in a gloss from a
// diffuse surface, would only sparkle at a reference's sample counts).
bool velk_shade_highlights = true;

BrdfSample velk_pbr_shade(MaterialEval eval, EvalContext ctx)
{
    vec3 N = normalize(eval.normal);
    vec3 V = normalize(-ctx.ray_dir);
    float metallic  = clamp(eval.metallic, 0.0, 1.0);
    float roughness = clamp(eval.roughness, 0.04, 1.0);
    vec3 base = eval.color.rgb;
    float ao = clamp(eval.occlusion, 0.0, 1.0);
    float transmission = velk_hit_two_sided ? clamp(eval.transmission, 0.0, 1.0) : 0.0;

    // KHR_materials_specular, as the deferred lighting's velk_specular_f0: a dielectric reflects
    // its strength times 0.04 tinted by its colour (capped at 1) at normal incidence and its
    // strength at grazing; a metal its base colour, and 1.
    float specular = clamp(eval.specular_factor, 0.0, 1.0);
    vec3 F0 = mix(min(0.04 * max(eval.specular_color_factor, vec3(0.0)), vec3(1.0)) * specular, base, metallic);
    float F90 = mix(specular, 1.0, metallic);
    float VdotN = max(dot(V, N), 0.0);
    vec3 F = F0 + (vec3(F90) - F0) * pow(1.0 - VdotN, 5.0);

    // Direct lighting from scene lights. Each light contributes a
    // Lambertian diffuse term scaled by its distance / spot attenuation
    // and modulated by its shadow technique's visibility.
    vec3 direct = vec3(0.0);
    // Single-light mode. -1 (the default) shades against every light, which is
    // what primary RT rays want. The reference path sets one index and the
    // reciprocal of the probability it was chosen with: its hits accumulate over
    // many frames, so one light per hit converges to the same answer while
    // keeping the ray count per hit independent of how many lights there are.
    uint li_begin = (velk_single_light >= 0) ? uint(velk_single_light) : 0u;
    uint li_end   = (velk_single_light >= 0) ? uint(velk_single_light) + 1u : pc.light_count;
    float li_weight = (velk_single_light >= 0) ? velk_single_light_weight : 1.0;
    for (uint li = li_begin; li < li_end; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, ctx.rel_pos, L, atten);
        // A thin sheet passing `transmission` of its diffuse light takes that share from
        // behind it, its shadow ray leaving the side the light is on.
        float facing = dot(N, L);
        float lobe = facing > 0.0 ? (1.0 - transmission) * facing : transmission * -facing;
        if ((lobe <= 0.0 && facing <= 0.0) || atten <= 0.0) continue;
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.rel_pos, facing > 0.0 ? N : -N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += base * (1.0 - metallic) * lobe * radiance * li_weight;
        // Its highlight, on the side the light is on, as the deferred lighting's: GGX, Smith and
        // Schlick, times pi, since a light's intensity is the irradiance it gives over pi.
        // Continuing rays never meet a light, so this is its only highlight.
        if (facing > 0.0 && velk_shade_highlights) {
            vec3 Hl = normalize(L + V);
            float NdotH = max(dot(N, Hl), 0.0);
            float VdotH = max(dot(V, Hl), 0.0);
            float a2 = roughness * roughness * roughness * roughness;
            float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float D = a2 / max(3.14159265 * dd * dd, 1e-6);
            float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
            float G = VdotN / max(VdotN * (1.0 - k) + k, 1e-6) * facing / max(facing * (1.0 - k) + k, 1e-6);
            vec3 Fl = F0 + (vec3(F90) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
            direct += D * G * Fl / max(4.0 * VdotN * facing, 1e-6) * facing * radiance * li_weight * 3.14159265;
        }
    }

    // Diffuse term: crude "irradiance at the normal" via a single env
    // sample, modulated by AO. Upgrade to preconvolved irradiance or
    // stochastic cosine sampling when visibly needed.
    vec3 env_at_normal = transmission > 0.0 ? mix(env_miss_color(N), env_miss_color(-N), transmission) : env_miss_color(N);
    vec3 diffuse = base * (1.0 - metallic) * env_at_normal * ao;
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    velk_hit_diffuse = kD * base * ao;
    velk_hit_transmission = transmission;

    // Specular: sample a GGX half-vector, reflect V around it. Main's
    // iterative loop evaluates the reflected ray and multiplies by F.
    vec3 H = ggx_sample_half(N, roughness, rng_next_vec2());
    vec3 Lr = reflect(-V, H);

    BrdfSample bs;
    bs.emission = vec4(kD * diffuse + direct + eval.emissive, eval.color.a);
    bs.throughput = F;
    bs.next_dir = Lr;
    bs.terminate = false;
    // Sample count scales with GGX lobe width: 1 at mirror, up to 16 at
    // fully rough. Tracer clamps against its own per-bounce cap.
    bs.sample_count_hint = uint(1.0 + roughness * roughness * 15.0);
    return bs;
}
)";

// Compute ray tracer main body.
//
// Primary visibility runs the classic painter's loop (iterate all shapes
// in CPU-sorted depth order, composite each with its alpha over the
// accumulator) so 2D UI with stacked semi-transparent cards matches the
// rasterizer. Each primary hit evaluates its material; if the material
// returns a bounce (terminate == false, i.e. StandardMaterial), we run a
// small iterative secondary loop for that hit's specular path. GLSL has
// no recursion, so bounces can't be a function — they live inline here.
[[maybe_unused]] constexpr string_view rt_compute_main_src = R"(
const int kMaxBounces = 4;

vec3 trace_bounce(Ray ray, vec3 throughput)
{
    vec3 acc = vec3(0.0);
    for (int d = 1; d < kMaxBounces; ++d) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            acc += throughput * env_miss_color(ray.dir);
            return acc;
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, velk_hit_shading_normal(hit, ray.dir), material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        float a = clamp(bs.emission.a, 0.0, 1.0);
        acc += throughput * bs.emission.rgb * a;
        if (bs.terminate) {
            if (a >= 0.999) return acc;
            // Transparent: continue straight through, reduced throughput.
            throughput *= (1.0 - a);
            ray.origin = ctx.rel_pos +ray.dir * 1e-3;
            continue;
        }
        throughput *= bs.throughput;
        ray.origin = ctx.rel_pos +hit.normal * 1e-3;
        ray.dir = bs.next_dir;
    }
    // Bounce budget exhausted. The remaining throughput is illuminated by
    // the environment along whatever direction we were about to trace;
    // without this the mirror-of-mirror chain would blackhole.
    acc += throughput * env_miss_color(ray.dir);
    return acc;
}

// What a primary ray sees: the nearest surface lit, and through a translucent one what is
// behind it, blended by its alpha.
vec3 trace_primary(Ray ray)
{
    const int kMaxLayers = 8;
    vec3 accum = vec3(0.0);
    vec3 throughput = vec3(1.0);
    for (int layer = 0; layer < kMaxLayers; ++layer) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            return accum + throughput * env_miss_color(ray.dir);
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, velk_hit_shading_normal(hit, ray.dir), material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        vec3 rgb = bs.emission.rgb;
        if (!bs.terminate) {
            // Per-hit sample count: material's preferred count (scales with roughness / lobe
            // width), clamped by the tracer's global cap. Mirror surfaces collapse to 1; rough
            // surfaces spend the budget to flatten GGX noise.
            const uint kSppCap = 12u;
            uint spp = clamp(bs.sample_count_hint, 1u, kSppCap);
            vec3 bounce = vec3(0.0);
            Ray refl;
            refl.origin = ctx.rel_pos + hit.normal * 1e-3;
            refl.dir = bs.next_dir;
            bounce += trace_bounce(refl, bs.throughput);
            for (uint sp = 1u; sp < spp; ++sp) {
                BrdfSample bs2 = velk_resolve_fill(material, ctx);
                refl.dir = bs2.next_dir;
                bounce += trace_bounce(refl, bs2.throughput);
            }
            rgb += bounce * (1.0 / float(spp));
        }
        float a = clamp(bs.emission.a, 0.0, 1.0);
        accum += throughput * rgb * a;
        if (a >= 0.999) return accum;
        throughput *= 1.0 - a;
        ray.origin = ctx.rel_pos + ray.dir * 1e-3;
    }
    return accum;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.y;
    uint h = pc.extras.z;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    rng_init(uvec2(coord), VELK_GLOBALS.present_counter);

    // Per-pixel primary sample count. Each sample fires one jittered
    // primary ray; the results are averaged. Reduces edge-aliasing on shape boundaries and damps
    // bounce noise from rough specular hits. Cost scales linearly
    // with kPrimarySamples; 1 = no AA (raw stochastic output),
    // 4 = typical quality / cost balance.
    const uint kPrimarySamples = 4u;

    vec3 final = vec3(0.0);
    for (uint psample = 0u; psample < kPrimarySamples; ++psample) {
        // Sub-pixel jitter. Uniform 0..1 offset into the pixel; with
        // multiple samples the average lands near the pixel center
        // while still anti-aliasing edges. RNG is seeded per coord
        // + frame so jitter patterns are stable within a frame and
        // different between frames.
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h  = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        final += trace_primary(primary);
    }

    imageStore(gStorageImages[nonuniformEXT(pc.extras.x)], coord,
               vec4(final * (1.0 / float(kPrimarySamples)), 1.0));
}
)";

/// Path tracing for reference images. Goes after compose_rt_compute's shared body. Each frame
/// traces one path for a share of the pixels and adds it to a running sum, so the cost of a
/// frame stays bounded. `extras` is the sum image (RGBA32F, alpha counts paths), the image
/// shown (RGBA16F), width and height. `shapes_base` is nonzero to start over. `env.y` is the
/// environment texture, and `env.z` and `env.w` its intensity and rotation as bits. `env.x`
/// must be 0, so that a surface is not lit by the environment along its normal as well.
#define VELK_REFERENCE_PATH_GLSL R"(
// The brightest the sky may count. Readouts lower it so that a tiny sun cannot dominate them.
float reference_sky_cap = 1e30;

vec3 reference_sky(vec3 rd)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float rotation = uintBitsToFloat(pc.env.w);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    return min(textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, 0.0).rgb * uintBitsToFloat(pc.env.z), vec3(reference_sky_cap));
}

vec3 reference_cosine(vec3 n)
{
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    float v = rng_next_float();
    float angle = rng_next_float() * 6.28318531;
    float radius = sqrt(v);
    return t * (radius * cos(angle)) + b * (radius * sin(angle)) + n * sqrt(1.0 - v);
}

// One light per surface, in proportion to what it adds there.
void reference_pick_light(vec3 pos, vec3 n)
{
    velk_single_light = -1;
    velk_single_light_weight = 1.0;
    float w_sum = 0.0;
    float w_chosen = 0.0;
    for (uint li = 0u; li < pc.light_count; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, pos, L, atten);
        vec3 rad = light.color_intensity.rgb * light.color_intensity.a;
        float lw = (rad.r + rad.g + rad.b) * atten * max(dot(n, L), 0.05);
        if (lw <= 0.0) continue;
        w_sum += lw;
        if (rng_next_float() * w_sum <= lw) {
            velk_single_light = int(li);
            w_chosen = lw;
        }
    }
    if (w_chosen > 0.0) velk_single_light_weight = w_sum / w_chosen;
}

float reference_luma(vec3 c)
{
    return c.r + c.g + c.b;
}

// The light a path brings back, and in `first` what its first hit or miss alone brought.
vec3 reference_path(Ray ray, out vec3 first)
{
    const uint kBounces = 12u;
    first = vec3(0.0);
    vec3 total = vec3(0.0);
    vec3 carried = vec3(1.0);
    velk_shade_highlights = true;
    for (uint bounce = 0u; bounce < kBounces; ++bounce) {
        RayHit hit;
        if (!trace_closest_hit(ray, 1e30, hit)) {
            total += carried * reference_sky(ray.dir);
            if (bounce == 0u) first = total;
            break;
        }
        uint material;
        vec3 shading = velk_hit_shading_normal(hit, ray.dir);
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, shading, material);

        reference_pick_light(ctx.rel_pos, shading);
        velk_hit_diffuse = vec3(0.0);
        velk_hit_transmission = 0.0;
        BrdfSample bs = velk_resolve_fill(material, ctx);

        // Partly transparent: either the ray goes straight through, or the surface counts in full.
        if (rng_next_float() >= clamp(bs.emission.a, 0.0, 1.0)) {
            ray.origin = ctx.rel_pos + ray.dir * 0.01;
            continue;
        }
        total += carried * bs.emission.rgb;
        if (bounce == 0u) first = total;
        if (bs.terminate) break;

        // The side of its plane the ray came from, and the normal it was shaded with turned to it.
        vec3 side = dot(hit.plane, ray.dir) > 0.0 ? -hit.plane : hit.plane;
        vec3 n = dot(shading, side) < 0.0 ? -shading : shading;
        float diffuse_part = reference_luma(velk_hit_diffuse);
        float specular_part = reference_luma(bs.throughput);
        if (diffuse_part + specular_part < 1e-4) break;
        float specular_share = specular_part / (diffuse_part + specular_part);
        if (rng_next_float() < specular_share) {
            carried *= bs.throughput / specular_share;
            ray.dir = bs.next_dir;
            if (dot(ray.dir, side) <= 0.0) break;
        } else {
            carried *= velk_hit_diffuse / (1.0 - specular_share);
            velk_shade_highlights = false;
            // Through a thin sheet as often as it passes light through: to its other side.
            if (rng_next_float() < velk_hit_transmission) {
                n = -n;
                side = -side;
            }
            ray.dir = reference_cosine(n);
            if (dot(ray.dir, side) <= 0.0) break;
        }
        ray.origin = ctx.rel_pos + side * 0.01;
        if (max(carried.r, max(carried.g, carried.b)) < 1e-3) break;
    }
    return total;
}
)"

inline constexpr string_view rt_reference_main_src = VELK_REFERENCE_PATH_GLSL VELK_SUN_VISIBILITY_GLSL R"(
// VELK_SUN_VISIBILITY's modes, in pc.more.x: 1 for the sun's visibility over its disk at the
// surface seen through each pixel's middle, 2 for that of the ray to its middle alone. The first
// surface a pixel sees is the first not mostly clear, as the deferred path draws it.
float reference_sun_visibility(Ray ray, bool whole_disk)
{
    uint sun = velk_sun_index();
    if (sun == 0xFFFFFFFFu) return 1.0;
    for (uint layer = 0u; layer < 8u; ++layer) {
        RayHit hit;
        if (!trace_closest_hit(ray, 1e30, hit)) return 1.0;
        uint material;
        vec3 shading = velk_hit_shading_normal(hit, ray.dir);
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, shading, material);
        reference_pick_light(ctx.rel_pos, shading);
        velk_hit_diffuse = vec3(0.0);
        velk_hit_transmission = 0.0;
        BrdfSample bs = velk_resolve_fill(material, ctx);
        if (bs.emission.a < 0.5) {
            ray.origin = ctx.rel_pos + ray.dir * 0.01;
            continue;
        }
        // The normal it was shaded with, turned to the side the pixel sees.
        vec3 side = dot(hit.plane, ray.dir) > 0.0 ? -hit.plane : hit.plane;
        vec3 n = dot(shading, side) < 0.0 ? -shading : shading;
        Light light = velk_light_at(pc.lights_base + sun);
        vec3 dir = -light.direction.xyz;
        if (whole_disk) {
            vec3 a = normalize(abs(dir.y) < 0.9 ? cross(dir, vec3(0.0, 1.0, 0.0)) : cross(dir, vec3(1.0, 0.0, 0.0)));
            vec3 b = cross(dir, a);
            float spread = tan(max(light.params.w, 0.0)) * sqrt(rng_next_float());
            float turn = 6.2831853 * rng_next_float();
            dir = normalize(dir + (a * cos(turn) + b * sin(turn)) * spread);
        }
        float blocker_t;
        bool facing_away;
        return velk_sun_visibility(ctx.rel_pos, n, velk_hit_transmission > 0.0, hit.stand_in >= 0.0, dir, blocker_t, facing_away);
    }
    return 1.0;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.z;
    uint h = pc.extras.w;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    uint frame = VELK_GLOBALS.present_counter;
    vec4 held = pc.shapes_base != 0u ? vec4(0.0) : texelFetch(velk_textures[nonuniformEXT(pc.extras.x)], coord, 0);

    // The sun's visibility, at every pixel every frame, through its middle.
    if (pc.more.x != 0u) {
        rng_init(uvec2(coord), frame);
        vec2 ndc = (vec2(coord) + 0.5) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        Ray primary;
        primary.origin = near_h.xyz / near_h.w;
        primary.dir = normalize(far_h.xyz / far_h.w - primary.origin);
        held += vec4(vec3(reference_sun_visibility(primary, pc.more.x == 1u)), 1.0);
        imageStore(gStorageImagesF32[nonuniformEXT(pc.extras.x)], coord, held);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.y)], coord, vec4(held.rgb / max(held.a, 1.0), 1.0));
        return;
    }

    const uint kShare = 8u;
    if ((uint(coord.x) + uint(coord.y) * 3u + frame) % kShare == 0u) {
        rng_init(uvec2(coord), frame);
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w = far_h.xyz / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        vec3 first_hit;
        vec3 brought = reference_path(primary, first_hit);
        if (!any(isnan(brought)) && !any(isinf(brought))) {
            held += vec4(brought, 1.0);
        }
    }

    imageStore(gStorageImagesF32[nonuniformEXT(pc.extras.x)], coord, held);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.y)], coord, vec4(held.rgb / max(held.a, 1.0), 1.0));
}
)";

/// Measures a frame for render tests. Mode 0 clears this frame's words. Mode 1 follows each
/// surface back to the previous frame and adds up how much its lighting changed, so camera
/// motion does not count and surfaces that moved are skipped. `words` holds: valid, summed
/// change times 256, over 5%, over 20%, the same four for surfaces facing up or down, two
/// unused, then per screen tile valid and summed.
inline constexpr string_view test_diagnostics_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 1, binding = 16, std430) buffer TestWords { uint data[]; } test_words;

layout(push_constant, scalar) uniform PC {
    uint word;
    uint words;
    uint mode;
    uint globals_base;
    uint normal_id;
    uint pos_id;
    uint albedo_id;
    uint color_id;
    uint history_id;
    uint output_id;
    uint width;
    uint height;
    uint tiles_up_only;
    uint view_id;
} pc;

const uint kTilesX = 16u;
const uint kTilesY = 9u;
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

void add(uint at, uint amount) { atomicAdd(test_words.data[pc.word + at], amount); }

void main()
{
    if (pc.mode == 0u) {
        uint i = gl_LocalInvocationIndex + gl_WorkGroupID.x * 64u;
        if (i < pc.words) test_words.data[pc.word + i] = 0u;
        return;
    }
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    if (pc.view_id != 0u) imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(0.0, 0.0, 0.0, 1.0));

    vec3 normal = texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], p, 0).xyz;
    if (dot(normal, normal) < 1e-6) {
        imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(0.0));
        return;
    }
    vec3 N = normalize(normal);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], p, 0).xyz;
    vec3 albedo = texelFetch(velk_textures[nonuniformEXT(pc.albedo_id)], p, 0).rgb;
    vec3 color = texelFetch(velk_textures[nonuniformEXT(pc.color_id)], p, 0).rgb;
    float light = max(dot(color, kLuma) / max(dot(albedo, kLuma), 0.04), 1e-4);
    GlobalData g = velk_globals.data[pc.globals_base];
    if (pc.mode == 5u) {
        light = max(dot(albedo, kLuma), 1e-4);
    }
    imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(rel, light));

    vec3 prev_pos = rel + g.cam_moved;
    vec4 prev_clip = g.prev_view_projection * vec4(prev_pos, 1.0);
    if (prev_clip.w <= 1e-6) return;
    vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThanEqual(prev_uv, vec2(1.0)))) return;
    ivec2 q = ivec2(prev_uv * vec2(float(pc.width), float(pc.height)));
    ivec2 last = ivec2(int(pc.width) - 1, int(pc.height) - 1);

    // The range of the light on this surface around where it was. Reprojection lands between
    // pixels, so only light outside what was nearby counts as having changed.
    mat4 vp = g.view_projection;
    float focal = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixel = length(rel) * 2.0 / (focal * float(pc.height));
    float lowest = 1e30;
    float highest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 history = texelFetch(velk_textures[nonuniformEXT(pc.history_id)], clamp(q + ivec2(x, y), ivec2(0), last), 0);
            if (history.a > 0.0 && length(history.xyz - prev_pos) <= pixel * 6.0) {
                lowest = min(lowest, history.a);
                highest = max(highest, history.a);
            }
        }
    }
    if (highest <= 0.0) return;

    float nearest = clamp(light, lowest, highest);
    float change = abs(light - nearest) / max(max(light, nearest), 1e-3);
    // White where the surface faces up or down, blue elsewhere.
    if (pc.view_id != 0u) {
        float shown = min(change * 8.0, 1.0);
        float up = abs(N.y) > 0.9 ? shown : 0.0;
        imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(up, up, shown, 1.0));
    }
    uint scaled = uint(change * 256.0);
    add(0u, 1u);
    add(1u, scaled);
    if (change > 0.05) add(2u, 1u);
    if (change > 0.2) add(3u, 1u);
    if (abs(N.y) > 0.9) {
        add(4u, 1u);
        add(5u, scaled);
        if (change > 0.05) add(6u, 1u);
        if (change > 0.2) add(7u, 1u);
    }
    if (pc.tiles_up_only != 0u && abs(N.y) <= 0.9) return;
    uint tile = min(uint(p.y) * kTilesY / pc.height, kTilesY - 1u) * kTilesX + min(uint(p.x) * kTilesX / pc.width, kTilesX - 1u);
    add(10u + tile, 1u);
    add(10u + kTilesX * kTilesY + tile, scaled);
}
)";

/// Appends the #include lines of the intersect snippets registered for this frame.
inline void append_rt_intersect_includes(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto& intersect_info = snippets.intersect_info_by_id();
    for (auto id : snippets.frame_intersects()) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }
}

/// Appends intersect_shape: built-in rect/cube/sphere kinds and registered visual kinds (3+).
/// Triangle meshes are walked by velk_walk and never get here.
inline void append_rt_intersect_shape(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto append_literal = [&src](const char* s) { src += string_view(s, std::strlen(s)); };
    const auto& intersect_info = snippets.intersect_info_by_id();
    char buf[128];
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    for (auto id : snippets.frame_intersects()) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");
}

/**
 * @brief Composes a compute shader that traces rays and runs no material code: the RT prelude,
 *        the frame's intersect snippets and intersect_shape, then @p main_src. Its pipeline
 *        depends only on the frame's intersects. A work group is @p group_threads threads in a
 *        row; 0 keeps the prelude's 8 by 8.
 */
inline string compose_rt_trace(const IFrameSnippetRegistry& snippets, string_view main_src, uint32_t group_threads = 0)
{
    string src;
    if (group_threads != 0) {
        // The group size goes right after the #version line, before the prelude reads it.
        const size_t version = rt_compute_prelude_src.find(string_view("#version"));
        const size_t line_end = rt_compute_prelude_src.find('\n', version);
        src += rt_compute_prelude_src.substr(0, line_end + 1);
        char define[64];
        const int length = std::snprintf(define, sizeof(define), "#define VELK_RT_GROUP_THREADS %u\n", group_threads);
        // As long as written: a string_view of the whole array would take its NUL and what follows.
        src += string_view(define, length > 0 ? size_t(length) : 0u);
        src += rt_compute_prelude_src.substr(line_end + 1);
    } else {
        src += rt_compute_prelude_src;
    }
    append_rt_intersect_includes(src, snippets);
    append_rt_intersect_shape(src, snippets);
    src += main_src;
    return src;
}

/**
 * A point on a drawn triangle, the same for a pixel and a ray: from the draw (VelkDrawData),
 * the instance, the cluster and the triangle, then the weights of its corners, what its
 * material is given. Part of compose_rt_compute's shared body, after velk_material_varyings.
 */
inline constexpr string_view velk_surface_src = R"(
// A corner of a drawn triangle, as its material's vertex function gives it.
struct VelkSurfaceVertex {
    vec3 rel;       // in world space, from the camera
    vec3 normal;
    vec2 uv;
    vec2 uv1;
    vec4 tangent;
    vec4 color;
    uint shape_param;
    float normal_length;  // of its normal as authored, before the vertex function (velk_stand_in_flatness)
};

VelkSurfaceVertex velk_surface_vertex(VelkDrawData d, uint cluster, ElementInstance inst, GlobalData g, uint index)
{
    VelkVertex3D v = velk_unpack_vertex3d(velk_cluster_vertex_word(d, cluster) + index * VELK_VERTEX3D_WORDS);
    VelkVaryings s = velk_material_varyings(d.material_id, v, inst, g);
    VelkSurfaceVertex r;
    r.normal_length = length(v.normal);
    r.rel = s.rel_pos;
    r.normal = s.world_normal;
    r.uv = s.local_uv;
    r.uv1 = velk_read_uv1(velk_cluster_uv1_word(d, cluster, index));
    r.tangent = s.world_tangent;
    r.color = s.color * velk_read_color(velk_cluster_color_word(d, cluster, index));
    r.shape_param = s.shape_param;
    return r;
}

// The vertex indices of a triangle of a draw's cluster. A triangle strip's triangles start
// one vertex apart.
uvec3 velk_surface_corners(VelkDrawData d, uint cluster, uint triangle)
{
    if (d.index_base == VELK_NO_VISIBLE_LIST) return uvec3(triangle) + uvec3(0u, 1u, 2u);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    return uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
}

// What the material is given at the point with corner weights @p weights, at @p rel from the
// camera of @p g.
EvalContext velk_surface_context(VelkDrawData d, GlobalData g, VelkSurfaceVertex v0, VelkSurfaceVertex v1,
                                 VelkSurfaceVertex v2, vec3 weights, vec3 rel)
{
    EvalContext ctx;
    ctx.material_base = d.material_words;
    ctx.texture_id = d.texture_id;
    ctx.shape_param = v0.shape_param;
    ctx.uv = v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z;
    ctx.uv1 = v0.uv1 * weights.x + v1.uv1 * weights.y + v2.uv1 * weights.z;
    ctx.base = v0.color * weights.x + v1.color * weights.y + v2.color * weights.z;
    ctx.ray_dir = normalize(rel);
    ctx.normal = v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z;
    ctx.hit_pos = rel + g.cam_pos.xyz;
    ctx.rel_pos = rel;
    ctx.tangent = v0.tangent * weights.x + v1.tangent * weights.y + v2.tangent * weights.z;
    return ctx;
}

// What the material of what a ray hit is given, as a pixel's is: from the hit's draw,
// instance, cluster and corners (RayHit, velk_hit_from_words), with the ray's direction,
// where it hit from the camera, and the surface's normal there for an analytic shape.
// `material` is the id velk_eval_material takes, 0 when the hit names no draw.
EvalContext velk_hit_context(RayHit hit, vec3 ray_dir, vec3 rel, vec3 normal, out uint material)
{
    EvalContext ctx;
    ctx.material_base = 0u;
    ctx.texture_id = 0u;
    ctx.shape_param = 0u;
    ctx.uv = hit.uv;
    ctx.uv1 = hit.uv;
    ctx.base = vec4(1.0);
    ctx.normal = normal;
    ctx.tangent = vec4(0.0);
    material = 0u;
    velk_hit_two_sided = false;
    if (hit.draw != 0xFFFFFFFFu && hit.instance != 0xFFFFFFFFu) {
        VelkDrawData d = velk_draw_data.data[hit.draw];
        velk_hit_two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        // A quad's hit names the quad's word, as its pixel does.
        ElementInstance inst = (d.flags & VELK_DRAW_QUADS) != 0u ? velk_quad_instance(hit.instance) : velk_instances.data[hit.instance];
        material = d.material_id;
        if (hit.corners.x == VELK_NO_CORNERS) {
            ctx.material_base = d.material_words;
            ctx.texture_id = d.texture_id;
            ctx.shape_param = inst.params.x;
            ctx.base = inst.color;
        } else {
            GlobalData g = VELK_GLOBALS;
            VelkSurfaceVertex v0 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.x);
            VelkSurfaceVertex v1 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.y);
            VelkSurfaceVertex v2 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.z);
            ctx = velk_surface_context(d, g, v0, v1, v2, vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights), rel);
            // Its corners' normal, on the side `normal` says it is seen from
            // (velk_hit_shading_normal).
            if (dot(ctx.normal, normal) < 0.0) ctx.normal = -ctx.normal;
        }
    }
    ctx.ray_dir = ray_dir;
    ctx.rel_pos = rel;
    ctx.hit_pos = rel + VELK_GLOBALS.cam_pos.xyz;
    return ctx;
}
)";

/**
 * @brief Composes the full RT compute pipeline source from registered
 *        material / shadow / intersect snippets.
 *
 * RT-side equivalent of compose_eval_fragment. Unlike the raster driver
 * templates (which substitute a single material's eval function name
 * via `<%EVAL_FN%>`), the RT pipeline dispatches to every active
 * material in a switch — so the composer is a procedural builder rather
 * than a placeholder template. Sections, in order:
 *
 *   1. rt_compute_prelude_src                (shared prelude)
 *   2. material #include lines               (one per active material)
 *   3. shadow tech #include lines
 *   4. intersect #include lines
 *   5. velk_eval_shadow switch               (calls registered shadow snippets)
 *   6. rt_pbr_shade_src                      (uses velk_eval_shadow)
 *   7. velk_resolve_fill switch              (calls eval_fn, may call velk_pbr_shade)
 *   8. intersect_shape switch                (built-in kinds + registered snippets)
 *   9. rt_compute_main_src                   (primary loop + bounce logic)
 */
inline string compose_rt_compute(const IFrameSnippetRegistry& snippets,
                                 string_view main_src = {})
{
    const auto& material_ids        = snippets.frame_materials();
    const auto& shadow_tech_ids     = snippets.frame_shadow_techs();
    const auto& material_info       = snippets.material_info_by_id();
    const auto& shadow_tech_info    = snippets.shadow_tech_info_by_id();

    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }
    append_rt_intersect_includes(src, snippets);

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    char buf[128];

    // velk_eval_shadow: dispatch by shadow_tech id.
    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ti.fn_name;
        append_literal("(light_idx, world_pos, world_normal);\n");
    }
    append_literal("        default: return 1.0;\n");
    append_literal("    }\n");
    append_literal("}\n");

    // Shared PBR shading helper — defined after velk_eval_shadow (which
    // it calls) and before velk_resolve_fill (which calls it for Lit
    // materials).
    src += rt_pbr_shade_src;

    // velk_material_varyings: dispatch by material id to its vertex function.
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        append_literal("#include \"");
        src += material_info[id - 1].vertex_include;
        append_literal("\"\n");
    }
    append_literal("VelkVaryings velk_material_varyings(uint mid, VelkVertex3D v, ElementInstance inst, GlobalData g) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += material_info[id - 1].vertex_fn;
        append_literal("(v, inst, g);\n");
    }
    append_literal("        default: return velk_default_varyings(v, inst, g);\n");
    append_literal("    }\n");
    append_literal("}\n");
    src += velk_surface_src;

    // velk_eval_material: dispatch by material id, without shading.
    append_literal("MaterialEval velk_eval_material(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);\n");
    }
    append_literal("        default: {"
                   " MaterialEval e = velk_default_material_eval();"
                   " e.color = ctx.base;"
                   " e.normal = ctx.normal;"
                   " e.roughness = 0.5;"
                   " e.lighting_mode = VELK_LIGHTING_STANDARD;"
                   " return e; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    // velk_resolve_fill: dispatch by material id; route Lit through
    // velk_pbr_shade and Unlit straight to emission.
    append_literal("BrdfSample velk_resolve_fill(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: { MaterialEval e = ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);"
                       " if (e.lighting_mode == VELK_LIGHTING_STANDARD)"
                       " return velk_pbr_shade(e, ctx);"
                       " BrdfSample bs;"
                       " bs.emission = e.color;"
                       " bs.throughput = vec3(0.0);"
                       " bs.next_dir = vec3(0.0);"
                       " bs.terminate = true;"
                       " bs.sample_count_hint = 1u;"
                       " return bs; }\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; bs.sample_count_hint = 1u; return bs; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    append_rt_intersect_shape(src, snippets);

    // Everything above is the shared RT body. Callers wanting that machinery
    // under a different entry point (the light probes pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
