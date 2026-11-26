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

#velk_include VELK_DEFERRED_PUSH_CONSTANTS_GLSL
#velk_include VELK_SURFACE_LIGHT_GLSL
#include "velk_intersect.glsl"

// The walk's stack per thread, in shared memory: 48 for 64 threads is 12 KB. The light trace's
// log gives the deepest a frame's rays got.
#define VELK_TRACE_STACK_SIZE 48
#include "velk_trace.glsl"

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
