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
