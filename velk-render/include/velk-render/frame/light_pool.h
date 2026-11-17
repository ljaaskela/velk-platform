#ifndef VELK_RENDER_FRAME_LIGHT_POOL_H
#define VELK_RENDER_FRAME_LIGHT_POOL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace velk {

/// A view's light pool, in the culling arena: where the light of the kept cuts' records is kept
/// (kCutRecordWords, its last word). A record's light word is one of:
///   kLightNone      not read yet
///   kLightPending   being given its light, for the moment it takes
///   below kLightOneValue  the first page of its light
///   kLightOneValue and up  one value for the whole record, in the bits under kLightOneValue
/// Light is about kLightPixels apart on screen wherever it is. A record gets its light the first
/// time a pixel shows it (light_claim_src): one value for a cluster smaller than that on screen,
/// a whole mesh, a shape drawn by its own pass and a cluster of a mesh that deforms, whose
/// vertices are not its own. A larger cluster goes on the search list (light_search_src), which
/// gives it pages: where its instance has records for light only (kCutLightOnly), the coarser
/// clusters at the light's level, each of its vertices names the three points of the nearest
/// of their triangles, and those records get their own points; else it has a point per vertex
/// itself, being coarse enough. A quad record's light is on the node of its quad tree that is as
/// wide as kLightPixels over it, one value for all the quads under that node. A record's light
/// goes with it while its partition keeps its cut, and to its like when the partition chooses
/// again. Pages no record names, directly or through its vertices, are found free every frame
/// (light_mark_src, light_sweep_src).
///
/// The pool is a header of kLightPoolHeaderWords, then a stack of the free pages, then a bit per
/// page marking those in use, both for as many pages as the pool may grow to, then the search
/// list, and its pages in segments of kLightSegmentPages, each a region of its own, so that
/// growing moves no page. The header:
///   0  free pages on the stack
///   1  pages, in all the segments
///   2  pages asked for this frame that were not there (a record without asks again)
///   3  records with pages, as the last mark found
///   4  records with one value, likewise
///   5  segments
///   6  the word of the stack
///   7  the word of the marks
///   8  pages taken off the stack this frame, from its top down: the free stack is only read
///      while records take pages, so taking needs no more than counting
///   9  records put on the search list this frame, past its room when more asked
///   10 the word of the search list: for each record, its word and its light word before
///   11 of the records with pages, those that link (kLightLinked), as the last mark found
///   12 to 15  3, 4, 11 and 9 of the last frame, complete, for the log
///   16 on, the word of each segment
/// A record's first page starts with kLightPageHeaderWords: its cluster's vertices, kLightOwn or
/// kLightLinked, how many point words it has, then its other pages, at most kLightMaxPages in
/// all, kLightNone for none. Its point words follow, then those of its other pages in order:
/// with kLightOwn a point of light per vertex, kLightNone until lit; with kLightLinked three
/// words per vertex: the first page of the record for light only whose points it reads, their
/// numbers there (8 bits each, the first lowest), and the weights of the second and third
/// (unorm 16 bits each, the second lowest; the first's is what is left of 1).
inline constexpr uint32_t kLightNone = 0xFFFFFFFFu;
inline constexpr uint32_t kLightPending = 0xFFFFFFFEu;
inline constexpr uint32_t kLightOneValue = 0x80000000u;
inline constexpr uint32_t kLightOwn = 1u;
inline constexpr uint32_t kLightLinked = 0x80000000u;
inline constexpr uint32_t kLightPageWords = 64;
inline constexpr uint32_t kLightPageHeaderWords = 18;
inline constexpr uint32_t kLightMaxPages = 16;
inline constexpr uint32_t kLightSegmentPages = 16384;
inline constexpr uint32_t kLightMaxSegments = 64;
inline constexpr uint32_t kLightPoolSegmentWord = 16;
inline constexpr uint32_t kLightPoolHeaderWords = kLightPoolSegmentWord + kLightMaxSegments;
/// Records the search list takes a frame; more wait for the next.
inline constexpr uint32_t kLightSearchCapacity = 1024;  ///< Mirrors VELK_LIGHT_SEARCH_CAPACITY.

// Reading the pool, for a shader that defines VELK_LIGHT_WORDS as the culling arena's buffer.
#define VELK_LIGHT_POOL_GLSL R"(
#define VELK_LIGHT_NONE 0xFFFFFFFFu
#define VELK_LIGHT_PENDING 0xFFFFFFFEu
#define VELK_LIGHT_ONE_VALUE 0x80000000u
#define VELK_LIGHT_OWN 1u
#define VELK_LIGHT_LINKED 0x80000000u
#define VELK_LIGHT_PAGE_WORDS 64u
#define VELK_LIGHT_PAGE_HEADER_WORDS 18u
#define VELK_LIGHT_MAX_PAGES 16u
// kLightSearchCapacity.
#define VELK_LIGHT_SEARCH_CAPACITY 1024u

// The first word of page `page` of the pool at `pool`.
uint velk_light_page(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[pool + 16u + (page >> 14u)] + (page & 16383u) * VELK_LIGHT_PAGE_WORDS;
}

// The word of point word `p` of the record whose first page starts at word `first`.
uint velk_light_point_word(uint pool, uint first, uint p)
{
    uint at = VELK_LIGHT_PAGE_HEADER_WORDS + p;
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    uint page_word = k == 0u ? first : velk_light_page(pool, VELK_LIGHT_WORDS.data[first + 2u + k]);
    return page_word + at % VELK_LIGHT_PAGE_WORDS;
}

// The points of light vertex `vertex` of the record whose first page starts at `first` reads,
// as the words they are in, and their weights: its own point, or the three it links to. A
// word is VELK_LIGHT_NONE where there is no point.
void velk_light_points(uint pool, uint first, uint vertex, out uint words[3], out vec3 weights)
{
    words[0] = VELK_LIGHT_NONE;
    words[1] = VELK_LIGHT_NONE;
    words[2] = VELK_LIGHT_NONE;
    weights = vec3(1.0, 0.0, 0.0);
    if (vertex >= VELK_LIGHT_WORDS.data[first]) return;
    if (VELK_LIGHT_WORDS.data[first + 1u] != VELK_LIGHT_LINKED) {
        words[0] = velk_light_point_word(pool, first, vertex);
        return;
    }
    uint light = VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u)];
    if (light >= VELK_LIGHT_WORDS.data[pool + 1u]) return;
    uint points = VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u + 1u)];
    vec2 w = unpackUnorm2x16(VELK_LIGHT_WORDS.data[velk_light_point_word(pool, first, vertex * 3u + 2u)]);
    uint other = velk_light_page(pool, light);
    for (uint k = 0u; k < 3u; ++k) {
        uint p = (points >> (8u * k)) & 255u;
        if (p < VELK_LIGHT_WORDS.data[other]) words[k] = velk_light_point_word(pool, other, p);
    }
    weights = vec3(max(1.0 - w.x - w.y, 0.0), w.x, w.y);
}

// The node of the quad tree at `tree` over node `node` whose quad is as wide on screen as
// `light_pixels`, the light of all the quads under it: the topmost going up from `node` that is
// not wider. In world space; `cam` the camera.
uint velk_light_quad_node(uint tree, uint node, vec3 cam, float pixels_per_unit, float light_pixels)
{
    uint i = (node - tree) / VELK_QUAD_NODE_WORDS;
    for (uint step = 0u; step < 32u && i > 0u; ++step) {
        uint parent = (i - 1u) / 2u;
        uint w = tree + parent * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 10u]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 1u]),
                           uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 2u]));
        vec3 u = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 3u]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 4u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 5u]));
        vec3 v = vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 6u]), uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 7u]),
                      uintBitsToFloat(VELK_LIGHT_WORDS.data[w + 8u]));
        float near = length(center - cam) - radius;
        if (near <= 0.0 || 2.0 * max(length(u), length(v)) * pixels_per_unit > light_pixels * near) break;
        i = parent;
    }
    return tree + i * VELK_QUAD_NODE_WORDS;
}
)"

/**
 * Gives each record a pixel shows its light, the first time one does, as kLightNone says: one
 * value, a quad's node, or a place on the search list for a cluster wide enough for points, which
 * waits as kLightPending until light_search_src lays it out. A record with one value that has
 * grown to twice the light spacing goes on the list too. A thread per pixel. `light_pixels` is
 * kLightPixels.
 */
inline constexpr string_view light_claim_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

struct LightInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer LightInstances { LightInstance data[]; } light_instances;
layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint ids_word;
    uint width;
    uint height;
    uint pool_word;
    float light_pixels;
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// Puts `record`, now pending, on the search list with the light it had; with no room, gives that back.
void velk_light_search(uint record, uint was)
{
    uint at = atomicAdd(light_words.data[pc.pool_word + 9u], 1u);
    if (at >= VELK_LIGHT_SEARCH_CAPACITY) {
        light_words.data[record + 3u] = was;
        return;
    }
    uint list = light_words.data[pc.pool_word + 10u] + at * 2u;
    light_words.data[list] = record;
    light_words.data[list + 1u] = was;
}

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    uint entry = light_words.data[record];

    // A quad: one value on the node over it as wide as the light spacing.
    if (entry == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(light_words.data[record + 2u], light_words.data[record + 1u], g.cam_pos.xyz,
                                         pixels_per_unit, pc.light_pixels);
        if (light_words.data[node + 11u] == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[node + 11u], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        }
        return;
    }

    uint was = light_words.data[record + 3u];
    if (was == VELK_LIGHT_PENDING) return;
    // A whole mesh or a shape drawn by its own pass: one value.
    if (entry == 0xFFFFFFFFu) {
        if (was == VELK_LIGHT_NONE) atomicCompSwap(light_words.data[record + 3u], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        return;
    }
    // Pages are laid out once; the search decides how.
    if (was != VELK_LIGHT_NONE && was < VELK_LIGHT_ONE_VALUE) return;

    // A cluster: as wide on screen as it is now.
    uint cluster = light_words.data[entry + 2u] + light_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + 19u];
    mat4 world = light_instances.data[d.instances_base + velk_ids_slot(d, ids)].world_matrix;
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + 13u]), uintBitsToFloat(light_words.data[cluster + 14u]),
                    uintBitsToFloat(light_words.data[cluster + 15u]));
    float radius = uintBitsToFloat(light_words.data[cluster + 16u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);

    if (was == VELK_LIGHT_NONE) {
        if (atomicCompSwap(light_words.data[record + 3u], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) return;
        if (vertices == 0u || across < pc.light_pixels) {
            light_words.data[record + 3u] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_search(record, VELK_LIGHT_NONE);
        return;
    }
    // One value, grown to twice the light spacing: points.
    if (vertices == 0u || across < 2.0 * pc.light_pixels) return;
    if (atomicCompSwap(light_words.data[record + 3u], was, VELK_LIGHT_PENDING) != was) return;
    velk_light_search(record, was);
}
)";

/**
 * Lays out the records on the search list (light_claim_src), a work group each. The records of
 * the same instance and mesh entry for light only (kCutLightOnly) are found beside it in the
 * store, since an instance's records are written together. With none, the record is coarse
 * enough and gets a point per vertex (kLightOwn). Else each of them that has no light yet gets
 * its own points, and each vertex of the record names the three points of the nearest of their
 * triangles, with its weights there (kLightLinked), a thread per vertex. A record that cannot be
 * laid out this frame (no room, or a record for light only another group is laying out) gets
 * its light back and asks again.
 */
inline constexpr string_view light_search_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// How far along the store to look for an instance's records for light only, each way.
const uint kScan = 256u;
const uint kMaxLights = 32u;

shared bool s_linked;       // the record links to records for light only, a thread per vertex
shared uint s_page;         // then its first page
shared uint s_first;        // and that page's word
shared uint s_lights;
shared uint s_light_records[kMaxLights];
shared uint s_light_pages[kMaxLights];
shared uint s_index_base;

// Takes `count` pages off the free stack into `pages`, false when there are not that many.
bool velk_light_take(uint pool, uint count, out uint pages[VELK_LIGHT_MAX_PAGES])
{
    uint taken = atomicAdd(light_words.data[pool + 8u], count);
    uint free_pages = light_words.data[pool];
    if (taken + count > free_pages) {
        // Given back, so that a smaller take after it can still fit.
        atomicAdd(light_words.data[pool + 8u], 0u - count);
        return false;
    }
    uint stack = light_words.data[pool + 6u];
    for (uint k = 0u; k < VELK_LIGHT_MAX_PAGES; ++k) {
        pages[k] = k < count ? light_words.data[stack + free_pages - 1u - taken - k] : VELK_LIGHT_NONE;
    }
    return true;
}

// Pages for `vertices` vertices of `per_vertex` point words each, as `kind`, with its own points
// unlit; false, and the pages asked for, when there is no room.
bool velk_light_lay_out(uint vertices, uint kind, uint per_vertex, out uint first_page)
{
    uint pool = pc.pool_word;
    uint points = vertices * per_vertex;
    uint count = (VELK_LIGHT_PAGE_HEADER_WORDS + points + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    uint pages[VELK_LIGHT_MAX_PAGES];
    first_page = VELK_LIGHT_NONE;
    if (count > VELK_LIGHT_MAX_PAGES) return false;
    if (!velk_light_take(pool, count, pages)) {
        atomicAdd(light_words.data[pool + 2u], count);
        return false;
    }
    uint first = velk_light_page(pool, pages[0]);
    light_words.data[first] = vertices;
    light_words.data[first + 1u] = kind;
    light_words.data[first + 2u] = points;
    for (uint k = 1u; k < VELK_LIGHT_MAX_PAGES; ++k) light_words.data[first + 2u + k] = pages[k];
    if (kind == VELK_LIGHT_OWN) {
        for (uint p = 0u; p < points; ++p) light_words.data[velk_light_point_word(pool, first, p)] = VELK_LIGHT_NONE;
    }
    first_page = pages[0];
    return true;
}

vec3 velk_light_position(uint word)
{
    return vec3(uintBitsToFloat(velk_mesh_words.data[word]), uintBitsToFloat(velk_mesh_words.data[word + 1u]),
                uintBitsToFloat(velk_mesh_words.data[word + 2u]));
}

// The weights of the point of triangle a, b, c nearest to p.
vec3 velk_light_nearest(vec3 p, vec3 a, vec3 b, vec3 c)
{
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return vec3(1.0, 0.0, 0.0);
    vec3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return vec3(0.0, 1.0, 0.0);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        float v = d1 / max(d1 - d3, 1e-30);
        return vec3(1.0 - v, v, 0.0);
    }
    vec3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return vec3(0.0, 0.0, 1.0);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        float w = d2 / max(d2 - d6, 1e-30);
        return vec3(1.0 - w, 0.0, w);
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        float w = (d4 - d3) / max((d4 - d3) + (d5 - d6), 1e-30);
        return vec3(0.0, 1.0 - w, w);
    }
    float denom = 1.0 / max(va + vb + vc, 1e-30);
    float v = vb * denom;
    float w = vc * denom;
    return vec3(1.0 - v - w, v, w);
}

void main()
{
    uint pool = pc.pool_word;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    if (index >= light_words.data[pool + 9u]) return;
    uint list = light_words.data[pool + 10u] + index * 2u;
    uint record = light_words.data[list];
    uint was = light_words.data[list + 1u];
    uint entry = light_words.data[record];
    uint slot = light_words.data[record + 2u];
    uint clusters = light_words.data[entry + 2u];
    uint cluster = clusters + light_words.data[record + 1u] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + 19u];

    if (lane == 0u) {
        s_linked = false;
        s_lights = 0u;
        bool again = false;
        uint pages = light_words.data[pool + 1u];
        // The instance's records for light only, beside it: an instance's records are together.
        for (int dir = -1; dir <= 1; dir += 2) {
            for (uint k = 1u; k <= kScan; ++k) {
                if (dir < 0 && k * 4u > record) break;
                uint other = dir < 0 ? record - k * 4u : record + k * 4u;
                if (light_words.data[other + 2u] != slot) break;
                if (light_words.data[other] != (entry | VELK_CUT_LIGHT_ONLY)) continue;
                uint light = light_words.data[other + 3u];
                if (light == VELK_LIGHT_NONE) {
                    if (atomicCompSwap(light_words.data[other + 3u], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) {
                        again = true;
                        continue;
                    }
                    uint other_cluster = clusters + light_words.data[other + 1u] * VELK_LOD_CLUSTER_WORDS;
                    uint laid;
                    if (!velk_light_lay_out(light_words.data[other_cluster + 19u], VELK_LIGHT_OWN, 1u, laid)) {
                        light_words.data[other + 3u] = VELK_LIGHT_NONE;
                        again = true;
                        continue;
                    }
                    light_words.data[other + 3u] = laid;
                    light = laid;
                }
                if (light == VELK_LIGHT_PENDING) {
                    again = true;
                } else if (light < pages && s_lights < kMaxLights) {
                    s_light_records[s_lights] = other;
                    s_light_pages[s_lights] = light;
                    ++s_lights;
                }
            }
        }
        uint draw = light_words.data[entry + 13u];
        if (again || (s_lights != 0u && draw == 0xFFFFFFFFu)) {
            light_words.data[record + 3u] = was;
        } else if (s_lights == 0u) {
            // Coarse enough: its own points.
            uint laid;
            light_words.data[record + 3u] = velk_light_lay_out(vertices, VELK_LIGHT_OWN, 1u, laid) ? laid : was;
        } else {
            uint laid;
            if (velk_light_lay_out(vertices, VELK_LIGHT_LINKED, 3u, laid)) {
                s_page = laid;
                s_first = velk_light_page(pool, laid);
                s_index_base = velk_draw_data.data[draw].index_base;
                s_linked = true;
            } else {
                light_words.data[record + 3u] = was;
            }
        }
    }
    barrier();
    if (!s_linked) return;

    // Each vertex: the nearest point on the records for light only, all in the mesh's own space.
    uint vertex_word = light_words.data[cluster + 18u];
    for (uint v = lane; v < vertices; v += 64u) {
        vec3 p = velk_light_position(vertex_word + v * VELK_VERTEX3D_WORDS);
        float best = 1e30;
        uint best_page = VELK_LIGHT_NONE;
        uint best_points = 0u;
        vec3 best_weights = vec3(1.0, 0.0, 0.0);
        for (uint i = 0u; i < s_lights; ++i) {
            uint other = clusters + light_words.data[s_light_records[i] + 1u] * VELK_LOD_CLUSTER_WORDS;
            vec3 center = vec3(uintBitsToFloat(light_words.data[other + 13u]), uintBitsToFloat(light_words.data[other + 14u]),
                               uintBitsToFloat(light_words.data[other + 15u]));
            if (length(p - center) - uintBitsToFloat(light_words.data[other + 16u]) >= best) continue;
            uint triangles = light_words.data[other + 1u];
            uint first_index = s_index_base + light_words.data[other];
            uint other_vertices = light_words.data[other + 18u];
            for (uint t = 0u; t < triangles; ++t) {
                uint w = first_index + t * 3u;
                uvec3 k = uvec3(velk_mesh_words.data[w], velk_mesh_words.data[w + 1u], velk_mesh_words.data[w + 2u]);
                vec3 a = velk_light_position(other_vertices + k.x * VELK_VERTEX3D_WORDS);
                vec3 b = velk_light_position(other_vertices + k.y * VELK_VERTEX3D_WORDS);
                vec3 c = velk_light_position(other_vertices + k.z * VELK_VERTEX3D_WORDS);
                vec3 weights = velk_light_nearest(p, a, b, c);
                float d = length(p - (a * weights.x + b * weights.y + c * weights.z));
                if (d < best) {
                    best = d;
                    best_page = s_light_pages[i];
                    best_points = (k.x & 255u) | ((k.y & 255u) << 8u) | ((k.z & 255u) << 16u);
                    best_weights = weights;
                }
            }
        }
        light_words.data[velk_light_point_word(pool, s_first, v * 3u)] = best_page;
        light_words.data[velk_light_point_word(pool, s_first, v * 3u + 1u)] = best_points;
        light_words.data[velk_light_point_word(pool, s_first, v * 3u + 2u)] = packUnorm2x16(best_weights.yz);
    }
    memoryBarrierBuffer();
    barrier();
    if (lane == 0u) light_words.data[record + 3u] = s_page;
}
)";

/**
 * Marks the pages the records of a kept cut's current store name, and those of the records for
 * light only their vertices link to, and counts its records with pages and with one value. A
 * thread per record, dispatched by the cut's drawing counts.
 */
inline constexpr string_view light_mark_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint cut_word;
    uint pool_word;
} pc;

#define VELK_LIGHT_WORDS light_words
)" VELK_LIGHT_POOL_GLSL R"(
// Never past the pool's pages, whatever a record says.
void velk_light_mark(uint page)
{
    if (page >= light_words.data[pc.pool_word + 1u]) return;
    atomicOr(light_words.data[light_words.data[pc.pool_word + 7u] + (page >> 5u)], 1u << (page & 31u));
}

// A record's first page and its others.
void velk_light_mark_record(uint light)
{
    velk_light_mark(light);
    uint first = velk_light_page(pc.pool_word, light);
    for (uint k = 1u; k < VELK_LIGHT_MAX_PAGES; ++k) {
        uint page = light_words.data[first + 2u + k];
        if (page != VELK_LIGHT_NONE) velk_light_mark(page);
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    if (index >= light_words.data[cut + 28u]) return;
    uint parity = light_words.data[cut + 12u];
    uint light = light_words.data[light_words.data[cut + 17u + parity] + index * 4u + 3u];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return;
    if (light >= VELK_LIGHT_ONE_VALUE) {
        atomicAdd(light_words.data[pc.pool_word + 4u], 1u);
        return;
    }
    uint pages = light_words.data[pc.pool_word + 1u];
    if (light >= pages) return;
    atomicAdd(light_words.data[pc.pool_word + 3u], 1u);
    velk_light_mark_record(light);
    uint first = velk_light_page(pc.pool_word, light);
    if (light_words.data[first + 1u] != VELK_LIGHT_LINKED) return;
    atomicAdd(light_words.data[pc.pool_word + 11u], 1u);
    // What its vertices link to, which neighbouring vertices mostly share.
    uint last = VELK_LIGHT_NONE;
    uint vertices = min(light_words.data[first], 256u);
    for (uint v = 0u; v < vertices; ++v) {
        uint linked = light_words.data[velk_light_point_word(pc.pool_word, first, v * 3u)];
        if (linked != last && linked < pages) velk_light_mark_record(linked);
        last = linked;
    }
}
)";

/**
 * Puts every page no mark says is in use on the free stack, and clears the marks for the next
 * frame. A thread per 32 pages. Starts and ends the frame's marking: with `begin` set, one
 * thread empties the stack, the mark's counts and the search list instead.
 */
inline constexpr string_view light_sweep_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint begin;
} pc;

void main()
{
    uint pool = pc.pool_word;
    uint word = gl_GlobalInvocationID.x;
    if (pc.begin != 0u) {
        if (word == 0u) {
            light_words.data[pool + 12u] = light_words.data[pool + 3u];
            light_words.data[pool + 13u] = light_words.data[pool + 4u];
            light_words.data[pool + 14u] = light_words.data[pool + 11u];
            light_words.data[pool + 15u] = light_words.data[pool + 9u];
            light_words.data[pool] = 0u;
            light_words.data[pool + 2u] = 0u;
            light_words.data[pool + 3u] = 0u;
            light_words.data[pool + 4u] = 0u;
            light_words.data[pool + 8u] = 0u;
            light_words.data[pool + 9u] = 0u;
            light_words.data[pool + 11u] = 0u;
        }
        return;
    }
    uint pages = light_words.data[pool + 1u];
    if (word * 32u >= pages) return;
    uint marks = light_words.data[pool + 7u] + word;
    uint used = light_words.data[marks];
    uint free_pages = 0u;
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
    }
    uint at = light_words.data[pool + 6u] + atomicAdd(light_words.data[pool], free_pages);
    for (uint b = 0u; b < 32u && word * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) light_words.data[at++] = word * 32u + b;
    }
    light_words.data[marks] = 0u;
}
)";

/// A view's light pool (see kLightNone): its header with the stack, the marks and the search
/// list, and its segments.
struct LightPool
{
    ArenaRegion header;
    vector<ArenaRegion> segments;
    uint32_t max_segments = 0;     ///< As many as the device has room for.
    uint64_t check_frame = 0;      ///< When pages asked for were last read back.
    uint64_t log_frame = 0;        ///< When its counts were last logged.
    uint32_t recorded_pages = 0;   ///< What the culling recording sweeps.

    uint32_t word() const
    {
        return header.valid() ? static_cast<uint32_t>(header.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu;
    }
    uint32_t pages() const { return static_cast<uint32_t>(segments.size()) * kLightSegmentPages; }
};

/// Makes @p pool's header on its first use, with a segment, and as many segments as
/// @p device_bytes of device memory has room for at most: a thirty-second of it, up to
/// kLightMaxSegments.
inline bool ensure_light_pool(LightPool& pool, IGpuArena& arena, uint64_t device_bytes)
{
    if (pool.header.valid()) {
        return true;
    }
    constexpr uint64_t segment_bytes = uint64_t(kLightSegmentPages) * kLightPageWords * sizeof(uint32_t);
    const uint64_t room = device_bytes != 0 ? device_bytes / 32u / segment_bytes : 1u;
    pool.max_segments = static_cast<uint32_t>(room < 1u ? 1u : room > kLightMaxSegments ? kLightMaxSegments : room);
    // A development aid: VELK_LIGHT_POOL_SEGMENTS caps the pool, to see it run full.
    if (const char* cap = std::getenv("VELK_LIGHT_POOL_SEGMENTS")) {
        const long segments = std::strtol(cap, nullptr, 10);
        if (segments >= 1 && segments < static_cast<long>(pool.max_segments)) {
            pool.max_segments = static_cast<uint32_t>(segments);
        }
    }
    const uint32_t max_pages = pool.max_segments * kLightSegmentPages;
    const uint64_t list_word = uint64_t(kLightPoolHeaderWords) + max_pages + max_pages / 32u;
    const uint64_t words = list_word + uint64_t(kLightSearchCapacity) * 2u;
    pool.header = arena.alloc(words * sizeof(uint32_t));
    if (!pool.header.valid()) {
        return false;
    }
    const uint32_t base = pool.word();
    vector<uint32_t> initial;
    initial.resize(kLightPoolHeaderWords + max_pages / 32u, 0u);
    initial[6] = base + kLightPoolHeaderWords;
    initial[7] = base + kLightPoolHeaderWords + max_pages;
    initial[10] = base + static_cast<uint32_t>(list_word);
    // The header, then the marks, all clear.
    arena.write_at(pool.header.offset(), initial.data(), kLightPoolHeaderWords * sizeof(uint32_t));
    arena.write_at(pool.header.offset() + (uint64_t(kLightPoolHeaderWords) + max_pages) * sizeof(uint32_t),
                   initial.data() + kLightPoolHeaderWords, uint64_t(max_pages / 32u) * sizeof(uint32_t));
    pool.segments.clear();
    return true;
}

/// Adds @p count segments to @p pool, as many as it may have at most, and says so in its header.
/// Returns how many were added.
inline uint32_t grow_light_pool(LightPool& pool, IGpuArena& arena, uint32_t count)
{
    uint32_t added = 0;
    while (added < count && pool.segments.size() < pool.max_segments) {
        ArenaRegion segment = arena.alloc(uint64_t(kLightSegmentPages) * kLightPageWords * sizeof(uint32_t));
        if (!segment.valid()) {
            break;
        }
        const auto index = static_cast<uint32_t>(pool.segments.size());
        const auto word = static_cast<uint32_t>(segment.offset() / sizeof(uint32_t));
        arena.update_at(pool.header.offset() + (uint64_t(kLightPoolSegmentWord) + index) * sizeof(uint32_t), &word, sizeof(word));
        pool.segments.push_back(std::move(segment));
        ++added;
    }
    const uint32_t pages = pool.pages();
    arena.update_at(pool.header.offset() + sizeof(uint32_t), &pages, sizeof(pages));
    const auto segments = static_cast<uint32_t>(pool.segments.size());
    arena.update_at(pool.header.offset() + 5u * sizeof(uint32_t), &segments, sizeof(segments));
    return added;
}

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_H
