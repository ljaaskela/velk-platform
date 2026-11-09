#ifndef VELK_RENDER_MESH_DEFORM_SHADERS_H
#define VELK_RENDER_MESH_DEFORM_SHADERS_H

#include <velk/string_view.h>

namespace velk::impl {

/**
 * Runs a deformer's velk_mesh_deform over a stream's vertices. The prefix and main go
 * around the deformer's own source. The table at table_word holds params (vec4), the
 * largest allowed displacement, and a count of vertices that moved too far.
 */
inline constexpr string_view mesh_deform_prefix_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 14, std430) buffer DeformVertices { uint data[]; } deform_vertices;
layout(set = 1, binding = 16, std430) buffer DeformWords { uint data[]; } deform_words;

layout(push_constant) uniform PC {
    uint rest_word;
    uint vertex_count;
    uint vertex_word;
    uint table_word;
} pc;

float rest_word(uint base, uint i)
{
    return uintBitsToFloat(deform_words.data[base + i]);
}

)";

inline constexpr string_view mesh_deform_main_src = R"(

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.vertex_count) return;

    uint base = pc.rest_word + index * VELK_VERTEX3D_WORDS;
    VelkVertex3D rest;
    rest.position = vec3(rest_word(base, 0u), rest_word(base, 1u), rest_word(base, 2u));
    rest.normal = vec3(rest_word(base, 3u), rest_word(base, 4u), rest_word(base, 5u));
    rest.uv = vec2(rest_word(base, 6u), rest_word(base, 7u));
    rest.tangent = vec4(rest_word(base, 8u), rest_word(base, 9u), rest_word(base, 10u), rest_word(base, 11u));

    uint t = pc.table_word;
    vec4 params = vec4(uintBitsToFloat(deform_words.data[t]), uintBitsToFloat(deform_words.data[t + 1u]),
                       uintBitsToFloat(deform_words.data[t + 2u]), uintBitsToFloat(deform_words.data[t + 3u]));
    VelkVertex3D v = velk_mesh_deform(rest, index, params);

    if (length(v.position - rest.position) > uintBitsToFloat(deform_words.data[t + 4u])) {
        atomicAdd(deform_words.data[t + 5u], 1u);
    }

    uint out_base = pc.vertex_word + index * VELK_VERTEX3D_WORDS;
    deform_vertices.data[out_base + 0u] = floatBitsToUint(v.position.x);
    deform_vertices.data[out_base + 1u] = floatBitsToUint(v.position.y);
    deform_vertices.data[out_base + 2u] = floatBitsToUint(v.position.z);
    deform_vertices.data[out_base + 3u] = floatBitsToUint(v.normal.x);
    deform_vertices.data[out_base + 4u] = floatBitsToUint(v.normal.y);
    deform_vertices.data[out_base + 5u] = floatBitsToUint(v.normal.z);
    deform_vertices.data[out_base + 6u] = floatBitsToUint(v.uv.x);
    deform_vertices.data[out_base + 7u] = floatBitsToUint(v.uv.y);
    deform_vertices.data[out_base + 8u] = floatBitsToUint(v.tangent.x);
    deform_vertices.data[out_base + 9u] = floatBitsToUint(v.tangent.y);
    deform_vertices.data[out_base + 10u] = floatBitsToUint(v.tangent.z);
    deform_vertices.data[out_base + 11u] = floatBitsToUint(v.tangent.w);
}
)";

/**
 * Refits one primitive's tree from its vertices, in one dispatch with an invocation per
 * leaf. A leaf copies its triangles' vertices into its cluster's positions, which leaves
 * sharing a vertex write identically, and works out its bounds. It then walks up the tree,
 * writing the bounds into its side of the parent: the first child to arrive stops, and the
 * second takes the union of both sides on up, since by then both are written. Each node's
 * counter starts at zero.
 */
inline constexpr string_view mesh_refit_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 16) in;

layout(set = 1, binding = 12, std430) coherent buffer RefitNodes { VelkMeshNode data[]; } refit_nodes;
layout(set = 1, binding = 13, std430) buffer RefitClusterWords { uint data[]; } refit_cluster_words;
layout(set = 1, binding = 14, std430) readonly buffer RefitVertices { uint data[]; } refit_vertices;
layout(set = 1, binding = 16, std430) coherent buffer RefitWords { uint data[]; } refit_words;

layout(push_constant) uniform PC {
    uint node_base;
    uint word_base;
    uint vertex_base;
    uint leaf_word;
    uint leaf_count;
    uint parent_word;
    uint counter_word;
} pc;

// Trees are far shallower than this. It only keeps a bad parent table from hanging the GPU.
const uint kMaxDepth = 4096u;
const uint kNoParent = 0xFFFFFFFFu;

void main()
{
    uint leaf_index = gl_GlobalInvocationID.x;
    if (leaf_index >= pc.leaf_count) return;

    uint leaf = pc.word_base + refit_words.data[pc.leaf_word + leaf_index * 2u];
    uint link = refit_words.data[pc.leaf_word + leaf_index * 2u + 1u];
    uint first_triangle = refit_cluster_words.data[leaf];
    uint counts = refit_cluster_words.data[leaf + 1u];
    uint triangle_count = counts & 0xFFFFu;
    uint positions = pc.word_base + refit_cluster_words.data[leaf + 2u];
    uint ids = positions + (counts >> 16u) * 3u;

    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    for (uint t = 0u; t < triangle_count; ++t) {
        uint triangle = refit_cluster_words.data[pc.word_base + first_triangle + t];
        for (uint c = 0u; c < 3u; ++c) {
            uint local = (triangle >> (c * 8u)) & 0xFFu;
            uint vertex = pc.vertex_base + refit_cluster_words.data[ids + local] * VELK_VERTEX3D_WORDS;
            uvec3 bits = uvec3(refit_vertices.data[vertex], refit_vertices.data[vertex + 1u],
                               refit_vertices.data[vertex + 2u]);
            refit_cluster_words.data[positions + local * 3u] = bits.x;
            refit_cluster_words.data[positions + local * 3u + 1u] = bits.y;
            refit_cluster_words.data[positions + local * 3u + 2u] = bits.z;
            vec3 p = vec3(uintBitsToFloat(bits.x), uintBitsToFloat(bits.y), uintBitsToFloat(bits.z));
            lo = min(lo, p);
            hi = max(hi, p);
        }
    }
    for (uint depth = 0u; depth < kMaxDepth && link != kNoParent; ++depth) {
        uint parent = pc.node_base + (link >> 1u);
        if ((link & 1u) == 0u) {
            refit_nodes.data[parent].a = vec4(lo, hi.x);
            refit_nodes.data[parent].b.xy = hi.yz;
        } else {
            refit_nodes.data[parent].b.zw = lo.xy;
            refit_nodes.data[parent].c = vec4(lo.z, hi);
        }
        memoryBarrierBuffer();
        if (atomicAdd(refit_words.data[pc.counter_word + (link >> 1u)], 1u) == 0u) return;
        memoryBarrierBuffer();

        VelkMeshNode node = refit_nodes.data[parent];
        vec3 left_min = node.a.xyz;
        vec3 left_max = vec3(node.a.w, node.b.xy);
        vec3 right_min = vec3(node.b.zw, node.c.x);
        vec3 right_max = node.c.yzw;
        lo = min(left_min, right_min);
        hi = max(left_max, right_max);
        link = refit_words.data[pc.parent_word + (link >> 1u)];
    }
}
)";

} // namespace velk::impl

#endif // VELK_RENDER_MESH_DEFORM_SHADERS_H
