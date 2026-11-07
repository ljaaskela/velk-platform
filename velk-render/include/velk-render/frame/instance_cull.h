#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>

#include <algorithm>
#include <cstring>

namespace velk {

/// Slots in a chunk of a partition's list. A partition's last chunk is filled out with
/// kCullNoSlot.
inline constexpr uint32_t kCullChunkSlots = 64;
inline constexpr uint32_t kCullNoSlot = 0xFFFFFFFFu;
/// Words per partition in a partition table. See instance_partition_cull_src.
inline constexpr uint32_t kCullEntryWords = 8;
/// Words before the chunks in a chunk list. See instance_partition_cull_src.
inline constexpr uint32_t kCullChunkListHeaderWords = 4;

// Whether an instance is too small on screen to be worth drawing, in which case it goes
// through instance_splat_src instead. Each one changes over at its own size, so that no
// line shows where they do. `area` is its mean projected area, 0 if it never changes over.
#define VELK_INSTANCE_FAR_GLSL R"(
const float kFarInstancePixels = 2.0;

bool velk_instance_is_far(uint slot, float area, float clip_w, mat4 vp, float height)
{
    if (area <= 0.0 || clip_w <= 0.0) return false;
    float f = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixels = sqrt(area) * f * height * 0.5 / clip_w;
    float h = float((slot * 2654435761u) >> 16u) / 65536.0;
    return pixels * (0.7 + 0.6 * h) < kFarInstancePixels;
}
)"

/**
 * Tests a visual's partitions against the view and appends the chunks of those that
 * pass to a list, for instance_cull_compute_src to go through. A table entry holds the
 * partition's world bounds (min xyz and max xyz, as floats), the word of its first
 * chunk, and its chunk count, 0 for an unused entry. The list at list_word starts with
 * the group counts of the dispatch that follows, and its first word is the chunk count.
 */
inline constexpr string_view instance_partition_cull_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint table_word;
    uint entry_count;
    uint list_word;
    uint globals_base;
    float max_distance;
} pc;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.entry_count) return;

    uint entry = pc.table_word + index * 8u;
    uint chunks = cull_words.data[entry + 7u];
    if (chunks == 0u) return;

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + 0u]), uintBitsToFloat(cull_words.data[entry + 1u]),
                   uintBitsToFloat(cull_words.data[entry + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + 3u]), uintBitsToFloat(cull_words.data[entry + 4u]),
                   uintBitsToFloat(cull_words.data[entry + 5u]));

    GlobalData globals = velk_globals.data[pc.globals_base];
    if (pc.max_distance > 0.0) {
        vec3 away = max(max(lo - globals.cam_pos.xyz, globals.cam_pos.xyz - hi), vec3(0.0));
        if (length(away) > pc.max_distance) return;
    }

    // Outside if the corner furthest along a plane's normal is behind it. Near and far
    // use the wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 r0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 r1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 r2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 r3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    vec4 planes[6] = vec4[6](r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2);
    for (int p = 0; p < 6; ++p) {
        vec3 corner = mix(lo, hi, greaterThanEqual(planes[p].xyz, vec3(0.0)));
        if (dot(planes[p].xyz, corner) + planes[p].w < 0.0) return;
    }

    uint first = cull_words.data[entry + 6u];
    uint at = atomicAdd(cull_words.data[pc.list_word], chunks);
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[pc.list_word + 4u + at + c] = first + c * 64u;
    }
}
)";

/**
 * Tests each instance's bounding sphere against the view and appends the visible
 * slots to its mesh's list in the culling arena, which the vertex shader reads through
 * velk_instance. The table at table_word holds 12 words per mesh: its bounding sphere
 * (center xyz and radius, as floats), the count of visible slots, the word of the
 * list, the list's capacity, its mean projected area at scale 1, and where the mesh's
 * primitives drawn by cluster are: the word of their entries, and how many, as described
 * at instance_cluster_runs_src. Each cluster a visible instance uses is counted there.
 * Instances with a zero matrix have been removed.
 *
 * With chunks_word set, each work group goes through the chunk at that index of the
 * list, as filled by instance_partition_cull_src. Without it, all slots are tested.
 */
inline constexpr string_view instance_cull_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float max_distance;
    uint chunks_word;
    uint far_form;
} pc;
)" VELK_INSTANCE_FAR_GLSL R"(
void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (pc.chunks_word != 0xFFFFFFFFu) {
        slot = cull_words.data[cull_words.data[pc.chunks_word + gl_WorkGroupID.x] + gl_LocalInvocationIndex];
    }
    if (slot >= pc.slot_count) return;

    mat4 world = cull_instances.data[pc.instances_base + slot].world_matrix;
    if (world[3][3] == 0.0) return;

    uint table = pc.table_word + cull_instances.data[pc.instances_base + slot].params.y * 12u;
    vec4 sphere = vec4(uintBitsToFloat(cull_words.data[table + 0u]),
                       uintBitsToFloat(cull_words.data[table + 1u]),
                       uintBitsToFloat(cull_words.data[table + 2u]),
                       uintBitsToFloat(cull_words.data[table + 3u]));
    vec3 center = (world * vec4(sphere.xyz, 1.0)).xyz;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float radius = sphere.w * sqrt(scale2);

    GlobalData globals = velk_globals.data[pc.globals_base];
    if (pc.max_distance > 0.0 && length(center - globals.cam_pos.xyz) - radius > pc.max_distance) return;

    // The sphere against each clip plane. Planes are rows of the matrix added to or
    // taken from its last row, so their normals' lengths are at most the sum of the
    // rows' lengths, which keeps this conservative. Near and far are tested with the
    // wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    if (abs(clip.x) > clip.w + w_reach + radius * length(r0)) return;
    if (abs(clip.y) > clip.w + w_reach + radius * length(r1)) return;
    if (abs(clip.z) > clip.w + w_reach + radius * length(r2)) return;

    if (pc.far_form != 0u) {
        float area = uintBitsToFloat(cull_words.data[table + 7u]) * scale2;
        if (velk_instance_is_far(slot, area, clip.w, vp, globals.viewport.y)) return;
    }

    uint index = atomicAdd(cull_words.data[table + 4u], 1u);
    if (index < cull_words.data[table + 6u]) {
        cull_words.data[cull_words.data[table + 5u] + index] = slot;
    }

    uint first_entry = cull_words.data[table + 8u];
    uint entries = cull_words.data[table + 9u];
    for (uint e = 0u; e < entries; ++e) {
        uint entry = first_entry + e * 12u;
        uint clusters = cull_words.data[entry + 3u];
        uint counts = cull_words.data[entry + 4u];
        for (uint c = 0u; c < clusters; ++c) {
            atomicAdd(cull_words.data[counts + c], 1u);
        }
    }
}
)";

/**
 * Lays out one primitive's draws by cluster, once its instances are counted. Each cluster in
 * use gets an indexed command, instanced over its own run of the list, so that instances
 * using the same cluster share a draw. An entry is 12 words: the word of the list, its
 * capacity, the word of the cluster table (kLodClusterWords words per cluster, starting
 * with the cluster's first index and triangle count), the cluster count, the words of the
 * counts, the cursors and the commands, one of each per cluster, how many commands are in use,
 * the word of its mesh in the table of instance_cull_compute_src, and the group counts of
 * the dispatch of instance_cluster_fill_src.
 */
inline constexpr string_view instance_cluster_runs_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint entry;
} pc;

void main()
{
    uint capacity = cull_words.data[pc.entry + 1u];
    uint table = cull_words.data[pc.entry + 2u];
    uint clusters = cull_words.data[pc.entry + 3u];
    uint counts = cull_words.data[pc.entry + 4u];
    uint cursors = cull_words.data[pc.entry + 5u];
    uint commands = cull_words.data[pc.entry + 6u];

    uint first = 0u;
    uint draws = 0u;
    for (uint c = 0u; c < clusters; ++c) {
        uint count = min(cull_words.data[counts + c], capacity - first);
        cull_words.data[counts + c] = 0u;
        cull_words.data[cursors + c] = first;
        if (count != 0u) {
            uint cluster = table + c * 13u;
            uint command = commands + draws * 5u;
            cull_words.data[command + 0u] = cull_words.data[cluster + 1u] * 3u;
            cull_words.data[command + 1u] = count;
            cull_words.data[command + 2u] = cull_words.data[cluster];
            cull_words.data[command + 3u] = 0u;
            cull_words.data[command + 4u] = first;
            ++draws;
        }
        first += count;
    }
    cull_words.data[pc.entry + 7u] = draws;

    uint mesh = cull_words.data[pc.entry + 8u];
    uint slots = min(cull_words.data[mesh + 4u], cull_words.data[mesh + 6u]);
    cull_words.data[pc.entry + 9u] = (slots + 63u) / 64u;
    cull_words.data[pc.entry + 10u] = 1u;
    cull_words.data[pc.entry + 11u] = 1u;
}
)";

/**
 * Goes through a mesh's visible slots and writes them to the runs of the clusters they use,
 * in one primitive's list. The clusters must be the ones instance_cull_compute_src counted.
 */
inline constexpr string_view instance_cluster_fill_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint entry;
} pc;

void main()
{
    uint mesh = cull_words.data[pc.entry + 8u];
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[mesh + 4u], cull_words.data[mesh + 6u])) return;
    uint slot = cull_words.data[cull_words.data[mesh + 5u] + index];

    uint list = cull_words.data[pc.entry + 0u];
    uint capacity = cull_words.data[pc.entry + 1u];
    uint clusters = cull_words.data[pc.entry + 3u];
    uint cursors = cull_words.data[pc.entry + 5u];
    for (uint c = 0u; c < clusters; ++c) {
        uint at = atomicAdd(cull_words.data[cursors + c], 1u);
        if (at < capacity) {
            cull_words.data[list + at] = slot;
        }
    }
}
)";

/**
 * Adds each instance too small to draw to the pixels it falls on, as its area in pixels,
 * shared over the 4 nearest so it moves smoothly. Goes through the chunks of the
 * partitions culling kept, or all slots without a chunk list. Instances behind something
 * nearer in the g-buffer are left out.
 */
inline constexpr string_view instance_splat_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct SplatInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer SplatInstances { SplatInstance data[]; } splat_instances;
layout(set = 1, binding = 16, std430) buffer SplatWords { uint data[]; } splat_words;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    uint chunks_word;
    uint words_word;
    uint width;
    uint height;
    uint worldpos_id;
    uint normal_id;
} pc;
)" VELK_INSTANCE_FAR_GLSL R"(
void add(ivec2 at, float amount)
{
    if (at.x < 0 || at.y < 0 || at.x >= int(pc.width) || at.y >= int(pc.height)) return;
    atomicAdd(splat_words.data[pc.words_word + uint(at.y) * pc.width + uint(at.x)], uint(amount * 4096.0));
}

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (pc.chunks_word != 0xFFFFFFFFu) {
        slot = splat_words.data[splat_words.data[pc.chunks_word + gl_WorkGroupID.x] + gl_LocalInvocationIndex];
    }
    if (slot >= pc.slot_count) return;
    mat4 world = splat_instances.data[pc.instances_base + slot].world_matrix;
    if (world[3][3] == 0.0) return;

    uint table = pc.table_word + splat_instances.data[pc.instances_base + slot].params.y * 12u;
    float mesh_area = uintBitsToFloat(splat_words.data[table + 7u]);
    if (mesh_area <= 0.0) return;
    vec3 local_center = vec3(uintBitsToFloat(splat_words.data[table + 0u]), uintBitsToFloat(splat_words.data[table + 1u]),
                             uintBitsToFloat(splat_words.data[table + 2u]));
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float area = mesh_area * scale2;

    GlobalData globals = velk_globals.data[pc.globals_base];
    mat4 vp = globals.view_projection;
    vec3 center = (world * vec4(local_center, 1.0)).xyz;
    vec4 clip = vp * vec4(center, 1.0);
    if (!velk_instance_is_far(slot, area, clip.w, vp, globals.viewport.y)) return;

    vec2 ndc = clip.xy / clip.w;
    if (any(greaterThan(abs(ndc), vec2(1.02)))) return;
    vec2 dims = vec2(float(pc.width), float(pc.height));
    vec2 px = (ndc * 0.5 + 0.5) * dims;

    vec2 uv = px / dims;
    vec3 n = velk_texture(pc.normal_id, uv).xyz;
    if (dot(n, n) > 1e-6) {
        float reach = uintBitsToFloat(splat_words.data[table + 3u]) * sqrt(scale2);
        vec3 p = velk_texture(pc.worldpos_id, uv).xyz;
        if (length(p - globals.cam_pos.xyz) < length(center - globals.cam_pos.xyz) - reach) return;
    }

    float f = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixels_per_unit = f * dims.y * 0.5 / clip.w;
    float pixels = area * pixels_per_unit * pixels_per_unit;

    vec2 q = px - 0.5;
    ivec2 base = ivec2(floor(q));
    vec2 t = q - vec2(base);
    add(base, pixels * (1.0 - t.x) * (1.0 - t.y));
    add(base + ivec2(1, 0), pixels * t.x * (1.0 - t.y));
    add(base + ivec2(0, 1), pixels * (1.0 - t.x) * t.y);
    add(base + ivec2(1, 1), pixels * t.x * t.y);
}
)";

/**
 * Covers the frame by what instance_splat_src added up. Instances overlap at random, so
 * coverage saturates rather than adds.
 *
 * An instance is lit from which way its mesh's surface faces: of the surface turned to the
 * viewer, how much is turned to the light, and how far. Indirect light comes from what
 * reaches the surface behind it, from the lighting pass's unshadowed image with that
 * surface's direct light taken out, or the nearest surface below for a pixel with only sky
 * behind it. The directions are kSurfaceDirections, and instances' own rotations are not
 * taken into account.
 *
 * Direct light is shadowed by what the lighting pass found for that surface: a share V of
 * it arrives after passing through the layer of instances, an optical depth tl = -ln V, and
 * the viewer sees through a depth tv, the coverage. The instances seen, weighted by how
 * much of each is visible, then get tv / (tv + tl) * (1 - e^-(tv + tl)) / (1 - e^-tv) of
 * the light. Seen from a shallow angle tv is large, which leaves the sunlit top of the layer.
 * That applies to surface among the instances, which is also hidden by them. Surface facing
 * out of their layer, along the normal of the surface behind, gets the light unshadowed.
 */
inline constexpr string_view instance_splat_composite_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 1, binding = 16, std430) readonly buffer SplatWords { uint data[]; } splat_words;

struct SplatLight {
    uvec4 flags;
    vec4 position;
    vec4 direction;
    vec4 color_intensity;
    vec4 params;
};
layout(set = 1, binding = 5, std430) readonly buffer SplatLights { SplatLight data[]; } splat_lights;

layout(push_constant) uniform PC {
    uint words_word;
    uint width;
    uint height;
    uint output_id;
    uint unshadowed_id;
    uint normal_id;
    uint worldpos_id;
    uint globals_base;
    uint lights_base;
    uint light_count;
    uint ratio_id;
    float albedo_r;
    float albedo_g;
    float albedo_b;
    float facing[14];
} pc;

const vec3 kDirections[14] = vec3[14](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
    vec3(0.57735, 0.57735, 0.57735), vec3(-0.57735, 0.57735, 0.57735), vec3(0.57735, -0.57735, 0.57735),
    vec3(-0.57735, -0.57735, 0.57735), vec3(0.57735, 0.57735, -0.57735), vec3(-0.57735, 0.57735, -0.57735),
    vec3(0.57735, -0.57735, -0.57735), vec3(-0.57735, -0.57735, -0.57735));

// Of the surface turned to the viewer, how far it is turned to the light on average, as
// the part from surface that is out in the open, and the part from surface among the other
// instances. Surface facing `up`, out of their layer, is not hidden by the others. Surface
// facing sideways is spread through the layer's depth, so only `seen_share` of it is seen.
vec2 facing_light(vec3 to_light, vec3 to_viewer, vec3 up, float seen_share)
{
    vec2 lit = vec2(0.0);
    float seen = 0.0;
    for (int d = 0; d < 14; ++d) {
        float open = max(dot(kDirections[d], up), 0.0);
        float towards_viewer = max(dot(kDirections[d], to_viewer), 0.0) * pc.facing[d];
        float towards_light = max(dot(kDirections[d], to_light), 0.0);
        float seen_open = towards_viewer * open;
        float seen_among = towards_viewer * (1.0 - open) * seen_share;
        seen += seen_open + seen_among;
        lit += vec2(seen_open, seen_among) * towards_light;
    }
    return seen > 0.0 ? lit / seen : vec2(0.0);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    // A pixel holds few instances, so its own count jumps as they cross its edges.
    // Neighbours are averaged in, which keeps the total the same.
    const float kTent[5] = float[5](1.0, 2.0, 3.0, 2.0, 1.0);
    float coverage = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 at = coord + ivec2(i, j);
            if (at.x < 0 || at.y < 0 || at.x >= int(pc.width) || at.y >= int(pc.height)) continue;
            coverage += float(splat_words.data[pc.words_word + uint(at.y) * pc.width + uint(at.x)]) * kTent[i + 2] * kTent[j + 2];
        }
    }
    coverage /= 4096.0 * 81.0;
    if (coverage <= 0.0) return;

    vec2 dims = vec2(float(pc.width), float(pc.height));
    vec2 uv = (vec2(coord) + 0.5) / dims;

    vec3 behind_normal = vec3(0.0);
    vec3 behind_pos = vec3(0.0);
    vec3 behind_light = vec3(0.0);
    vec3 behind_ratio = vec3(1.0);
    bool has_surface = false;
    for (int k = 0; k < 8 && !has_surface; ++k) {
        vec2 at = uv + vec2(0.0, float(k * k * 2)) / dims;
        vec3 n = velk_texture(pc.normal_id, at).xyz;
        if (dot(n, n) > 1e-6) {
            has_surface = true;
            behind_normal = normalize(n);
            behind_pos = velk_texture(pc.worldpos_id, at).xyz;
            behind_light = velk_texture(pc.unshadowed_id, at).rgb;
            behind_ratio = velk_texture(pc.ratio_id, at).rgb;
        }
    }

    GlobalData globals = velk_globals.data[pc.globals_base];
    // Through the near plane when there is no surface, where the matrix is most precise.
    vec3 towards = behind_pos;
    if (!has_surface) {
        vec4 near_point = globals.inverse_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        towards = near_point.xyz / near_point.w;
    }
    vec3 to_viewer = normalize(globals.cam_pos.xyz - towards);

    float to_viewer_depth = max(coverage, 1e-5);
    float seen_share = (1.0 - exp(-to_viewer_depth)) / to_viewer_depth;
    vec3 direct_open = vec3(0.0);
    vec3 direct_among = vec3(0.0);
    vec3 direct_on_behind = vec3(0.0);
    for (uint i = 0u; i < pc.light_count; ++i) {
        SplatLight light = splat_lights.data[pc.lights_base + i];
        if (light.flags.x == 3u) continue;
        vec3 L;
        float atten = 1.0;
        if (light.flags.x == 0u) {
            L = -light.direction.xyz;
        } else {
            if (!has_surface) continue;
            vec3 to_light = light.position.xyz - behind_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-6);
            float t = clamp(1.0 - dist / max(light.params.x, 1e-6), 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) {
                atten *= smoothstep(light.params.z, light.params.y, dot(-L, light.direction.xyz));
            }
        }
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten;
        vec2 facing = facing_light(L, to_viewer, behind_normal, seen_share);
        direct_open += radiance * facing.x;
        direct_among += radiance * facing.y;
        if (has_surface) {
            direct_on_behind += radiance * max(dot(behind_normal, L), 0.0);
        }
    }
    vec3 indirect = max(behind_light - direct_on_behind, vec3(0.0));

    // The ratio covers direct and indirect light together, and only direct gets shadowed.
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    float direct_behind = dot(direct_on_behind, kLuma);
    float arriving = 1.0;
    if (direct_behind > 1e-4) {
        float shadowed = dot(behind_ratio * behind_light, kLuma) - dot(indirect, kLuma);
        float to_light = -log(clamp(shadowed / direct_behind, 1e-3, 0.999));
        float both = to_light + to_viewer_depth;
        arriving = to_viewer_depth / both * (1.0 - exp(-both)) / (1.0 - exp(-to_viewer_depth));
    }
    vec3 direct_on_instance = direct_open + direct_among * arriving;

    vec4 under = velk_texture(pc.output_id, uv);
    vec3 surface = vec3(pc.albedo_r, pc.albedo_g, pc.albedo_b) * (direct_on_instance + indirect);    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], coord,
               vec4(mix(under.rgb, surface, 1.0 - exp(-coverage)), under.a));
}
)";

/**
 * @brief One view's GPU culling pass, kept by the render path. Set @c dirty when the
 *        view's batches change.
 */
struct InstanceCullPass
{
    IRenderPass::Ptr pass;
    bool dirty = true;
    vector<uint32_t> count_words;  ///< Reset before every cull.
    IGpuBuffer* arena_buffer = nullptr;
    uint64_t partition_tables = 0;  ///< Which tables the recording uses.
};

/**
 * @brief Adds @p batches' GPU culling to @p graph, if any of them are culled. Runs
 *        before the passes that draw them. With @p far_form, instances too small to draw
 *        are left out, for the path to add with instance_splat_src.
 */
inline void emit_instance_cull_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                    uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph,
                                    bool far_form = false)
{
    if (!batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }

    bool record = false;
    if (state.dirty) {
        state.dirty = false;
        record = true;
        state.count_words.clear();
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info)
                && std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                       == state.count_words.end()) {
                state.count_words.push_back(info.count_word);
            }
        }
    }
    if (state.count_words.empty()) {
        return;
    }

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    IGpuBuffer* arena_buffer = arena ? arena->buffer() : nullptr;
    if (!arena_buffer) {
        return;
    }
    // The copies below name the arena's buffer, which growing it replaces.
    record = record || arena_buffer != state.arena_buffer;
    state.arena_buffer = arena_buffer;

    // The BVH replaces a visual's tables when they fill up.
    uint64_t partition_tables = 0;
    for (const auto& batch : *batches) {
        InstanceCullInfo info{};
        if (batch && batch->get_instance_cull(info)) {
            partition_tables = partition_tables * 31u + info.partition_table_word;
            partition_tables = partition_tables * 31u + info.partition_count;
            partition_tables = partition_tables * 31u + info.chunk_list_word;
        }
    }
    record = record || partition_tables != state.partition_tables;
    state.partition_tables = partition_tables;

    const uint32_t zero = 0;
    for (uint32_t word : state.count_words) {
        arena->update_at(uint64_t(word) * sizeof(uint32_t), &zero, sizeof(zero));
    }

    emit_cached_view_pass(state.pass, record, "instance.cull", graph, [&](CachedPassRecording& rec) {
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_cull_compute_src, 0x4375'6c6c'496e'7374ULL);
        auto partition_pipeline = pipeline_for(instance_partition_cull_src, 0x4375'6c6c'5061'7274ULL);
        auto runs_pipeline = pipeline_for(instance_cluster_runs_src, 0x4375'6c6c'5275'6e73ULL);
        auto fill_pipeline = pipeline_for(instance_cluster_fill_src, 0x4375'6c6c'4669'6c6cULL);
        auto cmd = pipeline && partition_pipeline && runs_pipeline && fill_pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        cmd->begin_recording();
        cmd->push_label("Instance culling");
        // Earlier frames may still be drawing from these lists.
        cmd->record_global_barrier();

        vector<const void*> done;
        vector<InstanceCullInfo> visuals;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info)
                && std::find(done.begin(), done.end(), info.key) == done.end()) {
                done.push_back(info.key);
                visuals.push_back(info);
            }
        }

        // Partitions first, so whole partitions out of view cost nothing per instance.
        bool any_partitions = false;
        for (const auto& info : visuals) {
            if (info.partition_count != 0) {
                any_partitions = true;
                cmd->record_clear_buffer(arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t), sizeof(uint32_t));
            }
        }
        if (any_partitions) {
            cmd->record_global_barrier();
            struct PartitionPushC
            {
                uint32_t table_word;
                uint32_t entry_count;
                uint32_t list_word;
                uint32_t globals_base;
                float max_distance;
            };
            for (const auto& info : visuals) {
                if (info.partition_count == 0) {
                    continue;
                }
                const PartitionPushC pc{info.partition_table_word, info.partition_count, info.chunk_list_word,
                                        view_globals_base, info.max_distance};
                DispatchCall dc{};
                dc.pipeline = partition_pipeline.get();
                dc.groups_x = (info.partition_count + 63u) / 64u;
                dc.groups_y = 1;
                dc.groups_z = 1;
                dc.root_constants_size = sizeof(pc);
                std::memcpy(dc.root_constants, &pc, sizeof(pc));
                cmd->record_dispatch(dc);
            }
            cmd->record_global_barrier();
        }

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            uint32_t chunks_word;
            uint32_t far_form;
        };
        for (const auto& info : visuals) {
            const bool partitioned = info.partition_count != 0;
            const PushC pc{info.instances_base, info.slot_count, info.table_word, view_globals_base, info.max_distance,
                           partitioned ? info.chunk_list_word + kCullChunkListHeaderWords : kCullNoSlot,
                           far_form ? 1u : 0u};
            DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (info.slot_count + 63u) / 64u;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            if (partitioned) {
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t));
            } else {
                cmd->record_dispatch(dc);
            }
        }
        cmd->record_global_barrier();

        // Draws by cluster: lay out each one's runs, then fill them.
        vector<uint32_t> cluster_entries;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info) && info.cluster_count != 0
                && std::find(cluster_entries.begin(), cluster_entries.end(), info.cluster_entry_word) == cluster_entries.end()) {
                cluster_entries.push_back(info.cluster_entry_word);
            }
        }
        if (!cluster_entries.empty()) {
            constexpr uint32_t kFillGroupsWord = 9;
            for (uint32_t word : cluster_entries) {
                DispatchCall dc{};
                dc.pipeline = runs_pipeline.get();
                dc.groups_x = 1;
                dc.groups_y = 1;
                dc.groups_z = 1;
                dc.root_constants_size = sizeof(word);
                std::memcpy(dc.root_constants, &word, sizeof(word));
                cmd->record_dispatch(dc);
            }
            cmd->record_global_barrier();
            for (uint32_t word : cluster_entries) {
                DispatchCall dc{};
                dc.pipeline = fill_pipeline.get();
                dc.groups_x = 1;
                dc.groups_y = 1;
                dc.groups_z = 1;
                dc.root_constants_size = sizeof(word);
                std::memcpy(dc.root_constants, &word, sizeof(word));
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(word + kFillGroupsWord) * sizeof(uint32_t));
            }
            cmd->record_global_barrier();
        }

        // Each other batch's indirect args take the count of its list.
        constexpr uint64_t kInstanceCountOffset = BatchBufferLayout::kArgsOffset + sizeof(uint32_t);
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cluster_count != 0) {
                continue;
            }
            auto* entry = batch->storage_buffer() ? ctx.resources->find_buffer(batch->storage_buffer()) : nullptr;
            auto args = entry ? entry->buffer.lock() : nullptr;
            if (args) {
                cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * sizeof(uint32_t), args.get(),
                                        kInstanceCountOffset, sizeof(uint32_t));
            }
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(partition_pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
    });
}

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_H
