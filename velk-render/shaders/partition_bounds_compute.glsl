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
