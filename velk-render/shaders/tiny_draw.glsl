layout(local_size_x = 128) in;

struct TinyInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer TinyInstances { TinyInstance data[]; } tiny_instances;
layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;

layout(push_constant) uniform PC {
    uint list_word;
    uint planes_word;
    uint width;
    uint height;
    uint depth_id;  // the hardware's depth
    uint mode;
    uint most;
    uint wide_word;  // one pass: the 64-bit words, a pixel each
} pc;

#ifdef VELK_TINY_ONE_PASS
layout(set = 1, binding = 16, std430) buffer TinyWide { uint64_t data[]; } tiny_wide;
#endif

uint tiny_tie(uint record_index, uint triangle)
{
    uint h = record_index * 256u + triangle;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h & 0xFFu;
}

float tiny_edge(vec2 a, vec2 b, vec2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void main()
{
    uint index = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (index >= tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN]) return;
    uint record = pc.list_word + VELK_TINY_LIST_RECORDS + index * VELK_TINY_END;
    uint draw = tiny_words.data[record + VELK_TINY_DRAW];
    VelkDrawData d = velk_draw_data.data[draw];
    uint cluster = tiny_words.data[record + VELK_TINY_CLUSTER];
    TinyInstance inst = tiny_instances.data[d.instances_base + tiny_words.data[record + VELK_TINY_SLOT]];
    GlobalData g = velk_globals.data[d.globals_base];
    // The cluster's record says where its indices are, and the vertices they count from.
    uint first = tiny_words.data[cluster + VELK_LOD_CLUSTER_INDEX];
    uint vertices = tiny_words.data[cluster + VELK_LOD_CLUSTER_VERTEX];
    uint count = tiny_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES];
    vec2 size = vec2(float(pc.width), float(pc.height));
    uint plane = pc.width * pc.height;

    for (uint t = gl_LocalInvocationID.x; t < count; t += 128u) {
        vec3 s[3];
        bool behind = false;
        for (uint k = 0u; k < 3u; ++k) {
            uint at = vertices + velk_mesh_words.data[first + t * 3u + k] * VELK_VERTEX3D_WORDS;
            vec3 local = vec3(uintBitsToFloat(velk_mesh_words.data[at]), uintBitsToFloat(velk_mesh_words.data[at + 1u]),
                              uintBitsToFloat(velk_mesh_words.data[at + 2u]));
            vec4 clip = g.view_projection
                * vec4(velk_rel_position(inst.world_matrix, inst.offset.xyz + local * inst.size.xyz, g), 1.0);
            behind = behind || clip.w <= 0.0;
            s[k] = vec3((clip.xy / clip.w * 0.5 + 0.5) * size, clip.z / clip.w);
        }
        float area = tiny_edge(s[0].xy, s[1].xy, s[2].xy);
        if (behind || area == 0.0) continue;

        vec2 lo = min(min(s[0].xy, s[1].xy), s[2].xy);
        vec2 hi = max(max(s[0].xy, s[1].xy), s[2].xy);
        ivec2 from = max(ivec2(ceil(lo - 0.5)), ivec2(0));
        ivec2 to = min(ivec2(floor(hi - 0.5)), ivec2(int(pc.width), int(pc.height)) - 1);
        to = min(to, from + int(pc.most) - 1);
#ifdef VELK_TINY_ONE_PASS
        uint64_t payload = uint64_t((index << 8u) | t);
#else
        uint tie = tiny_tie(index, t);
#endif
        vec3 depths = vec3(s[0].z, s[1].z, s[2].z);
        for (int y = from.y; y <= to.y; ++y) {
            for (int x = from.x; x <= to.x; ++x) {
                vec2 p = vec2(float(x), float(y)) + 0.5;
                vec3 b = vec3(tiny_edge(s[1].xy, s[2].xy, p), tiny_edge(s[2].xy, s[0].xy, p), tiny_edge(s[0].xy, s[1].xy, p)) / area;
                if (b.x < 0.0 || b.y < 0.0 || b.z < 0.0) continue;
                float depth = dot(b, depths);
                if (depth <= 0.0 || depth > 1.0) continue;
                if (texelFetch(velk_textures[nonuniformEXT(pc.depth_id)], ivec2(x, y), 0).r >= depth) continue;
                uint pixel = uint(y) * pc.width + uint(x);
#ifdef VELK_TINY_ONE_PASS
                atomicMax(tiny_wide.data[pc.wide_word / 2u + pixel], (uint64_t(floatBitsToUint(depth)) << 32) | payload);
#else
                uint bits = (floatBitsToUint(depth) & ~0xFFu) | tie;
                if (pc.mode == 0u) {
                    atomicMax(tiny_words.data[pc.planes_word + pixel], bits);
                } else if (tiny_words.data[pc.planes_word + pixel] == bits) {
                    uint ids = pc.planes_word + plane + pixel * VELK_PIXEL_RECORD_WORDS;
                    tiny_words.data[ids] = (draw + 1u) | VELK_IDS_TINY | (t << VELK_IDS_TRIANGLE_SHIFT);
                    tiny_words.data[ids + 1u] = record;
                }
#endif
            }
        }
    }
}
