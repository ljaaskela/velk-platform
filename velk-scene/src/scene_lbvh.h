#ifndef VELK_SCENE_LBVH_H
#define VELK_SCENE_LBVH_H

#include <velk/string_view.h>

#include <cstdint>

namespace velk::impl {

/**
 * @brief Push constants of every stage of building a tree on the GPU.
 *
 * The tree goes in 2 * count - 1 nodes from node_base. The root is node 0, and each
 * inner node i has its two children at nodes 1 + 2i and 2 + 2i, since a walk needs a
 * node's children side by side. All word offsets are into the culling arena.
 */
struct LbvhPush
{
    uint32_t count;
    uint32_t items;           ///< 6 floats per item: its bounds, relative to the partition's origin.
    uint32_t keys_in;         ///< 2 words per item, least significant first.
    uint32_t values_in;
    uint32_t keys_out;
    uint32_t values_out;
    uint32_t histograms;      ///< 256 words per block of items.
    uint32_t digit_bases;     ///< 256 words: where each digit starts in the output.
    uint32_t shift;           ///< Of the digit being sorted on, within the whole key.
    uint32_t parents;         ///< Per node, the inner node it is a child of.
    uint32_t inner_nodes;     ///< Per inner node, the node it is placed at.
    uint32_t counters;        ///< Per inner node, how many children have arrived.
    uint32_t leaf_nodes;      ///< Per sorted item, the node it is placed at.
    uint32_t node_base;
    uint32_t record_base;     ///< Item i's record is record_base + i.
    float lo[3];              ///< Corner of the items' centers, for the Morton grid.
    float scale;              ///< Items' centers to the grid of kLbvhGridCells per axis.
};

/// Items a block of the radix sort holds. Blocks run in parallel, each in order on one
/// thread, so a block's length sets how long a sort takes. Same as kBlockItems below.
inline constexpr uint32_t kLbvhBlockItems = 256;
inline constexpr uint32_t kLbvhDigitBits = 8;
/// 21 bits per axis. One far item stretches the grid, so its cells must stay small enough
/// to tell the other items apart.
inline constexpr uint32_t kLbvhKeyBits = 63;
inline constexpr float kLbvhGridCells = 2097152.f;

inline constexpr string_view lbvh_prelude_src = R"(
#version 450
#include "velk.glsl"

layout(set = 1, binding = 0, std430) coherent buffer LbvhNodes { BvhNode data[]; } lbvh_nodes;
layout(set = 1, binding = 16, std430) coherent buffer LbvhWords { uint data[]; } lbvh_words;

layout(push_constant) uniform PC {
    uint count;
    uint items;
    uint keys_in;
    uint values_in;
    uint keys_out;
    uint values_out;
    uint histograms;
    uint digit_bases;
    uint shift;
    uint parents;
    uint inner_nodes;
    uint counters;
    uint leaf_nodes;
    uint node_base;
    uint record_base;
    float lo_x;
    float lo_y;
    float lo_z;
    float scale;
} pc;

#define WORDS lbvh_words.data
const uint kBlockItems = 256u;
const uint kNoParent = 0xFFFFFFFFu;

vec3 item_min(uint item)
{
    uint base = pc.items + item * 6u;
    return vec3(uintBitsToFloat(WORDS[base]), uintBitsToFloat(WORDS[base + 1u]), uintBitsToFloat(WORDS[base + 2u]));
}

vec3 item_max(uint item)
{
    uint base = pc.items + item * 6u + 3u;
    return vec3(uintBitsToFloat(WORDS[base]), uintBitsToFloat(WORDS[base + 1u]), uintBitsToFloat(WORDS[base + 2u]));
}
)";

/// Gives each item a 63-bit Morton key from its center.
inline constexpr string_view lbvh_keys_src = R"(
layout(local_size_x = 64) in;

// Puts two zero bits between the low 11 bits of v.
uint spread(uint v)
{
    uint top = (v >> 10u) & 1u;
    v &= 0x3FFu;
    v = (v | (v << 16u)) & 0x30000FFu;
    v = (v | (v << 8u)) & 0x300F00Fu;
    v = (v | (v << 4u)) & 0x30C30C3u;
    v = (v | (v << 2u)) & 0x9249249u;
    return v | (top << 30u);
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    vec3 center = (item_min(i) + item_max(i)) * 0.5;
    uvec3 cell = uvec3(clamp((center - vec3(pc.lo_x, pc.lo_y, pc.lo_z)) * pc.scale, vec3(0.0), vec3(2097151.0)));
    // Bits go x, y, z from the bottom, so the first word ends on y's bit 10, and z's bit
    // 10 is the first bit of the second.
    WORDS[pc.keys_in + i * 2u] = spread(cell.x & 0x7FFu) | (spread(cell.y & 0x7FFu) << 1u)
        | (spread(cell.z & 0x3FFu) << 2u);
    WORDS[pc.keys_in + i * 2u + 1u] = spread(cell.z >> 10u) | (spread(cell.x >> 11u) << 1u)
        | (spread(cell.y >> 11u) << 2u);
    WORDS[pc.values_in + i] = i;
}
)";

/// Counts each block's digits.
inline constexpr string_view lbvh_histogram_src = R"(
layout(local_size_x = 1) in;

void main()
{
    uint block = gl_GlobalInvocationID.x;
    uint row = pc.histograms + block * 256u;
    for (uint d = 0u; d < 256u; ++d) {
        WORDS[row + d] = 0u;
    }
    uint end = min(pc.count, (block + 1u) * kBlockItems);
    uint word = pc.shift >> 5u;
    uint shift = pc.shift & 31u;
    for (uint i = block * kBlockItems; i < end; ++i) {
        uint digit = (WORDS[pc.keys_in + i * 2u + word] >> shift) & 0xFFu;
        WORDS[row + digit] += 1u;
    }
}
)";

/// One invocation per digit: turns its counts into where each block's items start within
/// the digit, and leaves the digit's total.
inline constexpr string_view lbvh_offsets_src = R"(
layout(local_size_x = 1) in;

void main()
{
    uint digit = gl_GlobalInvocationID.x;
    uint blocks = (pc.count + kBlockItems - 1u) / kBlockItems;
    uint total = 0u;
    for (uint b = 0u; b < blocks; ++b) {
        uint at = pc.histograms + b * 256u + digit;
        uint n = WORDS[at];
        WORDS[at] = total;
        total += n;
    }
    WORDS[pc.digit_bases + digit] = total;
}
)";

/// Turns the digits' totals into where each digit starts.
inline constexpr string_view lbvh_digit_bases_src = R"(
layout(local_size_x = 1) in;

void main()
{
    uint total = 0u;
    for (uint digit = 0u; digit < 256u; ++digit) {
        uint n = WORDS[pc.digit_bases + digit];
        WORDS[pc.digit_bases + digit] = total;
        total += n;
    }
}
)";

/// Moves each block's items to their places, keeping their order.
inline constexpr string_view lbvh_scatter_src = R"(
layout(local_size_x = 1) in;

void main()
{
    uint block = gl_GlobalInvocationID.x;
    uint row = pc.histograms + block * 256u;
    uint end = min(pc.count, (block + 1u) * kBlockItems);
    uint word = pc.shift >> 5u;
    uint shift = pc.shift & 31u;
    for (uint i = block * kBlockItems; i < end; ++i) {
        uint low = WORDS[pc.keys_in + i * 2u];
        uint high = WORDS[pc.keys_in + i * 2u + 1u];
        uint digit = ((word == 0u ? low : high) >> shift) & 0xFFu;
        uint within = WORDS[row + digit];
        WORDS[row + digit] = within + 1u;
        uint to = WORDS[pc.digit_bases + digit] + within;
        WORDS[pc.keys_out + to * 2u] = low;
        WORDS[pc.keys_out + to * 2u + 1u] = high;
        WORDS[pc.values_out + to] = WORDS[pc.values_in + i];
    }
}
)";

/// Places each inner node's children, from the sorted keys alone. Each inner node finds
/// its own range and split, so they all run in parallel.
inline constexpr string_view lbvh_structure_src = R"(
layout(local_size_x = 64) in;

// How many leading bits two sorted items share. Equal keys fall back on their indices.
int common_bits(int a, int b)
{
    if (b < 0 || b >= int(pc.count)) return -1;
    uint high = WORDS[pc.keys_in + uint(a) * 2u + 1u] ^ WORDS[pc.keys_in + uint(b) * 2u + 1u];
    if (high != 0u) return 31 - findMSB(high);
    uint low = WORDS[pc.keys_in + uint(a) * 2u] ^ WORDS[pc.keys_in + uint(b) * 2u];
    if (low != 0u) return 32 + 31 - findMSB(low);
    return 64 + 31 - findMSB(uint(a) ^ uint(b));
}

void place_child(uint parent, uint node, bool is_leaf, uint index)
{
    WORDS[pc.parents + node] = parent;
    if (is_leaf) {
        WORDS[pc.leaf_nodes + index] = node;
        lbvh_nodes.data[pc.node_base + node].first_shape = pc.record_base + WORDS[pc.values_in + index];
        lbvh_nodes.data[pc.node_base + node].shape_count = 1u;
        lbvh_nodes.data[pc.node_base + node].first_child = 0u;
        lbvh_nodes.data[pc.node_base + node].child_count = 0u;
    } else {
        WORDS[pc.inner_nodes + index] = node;
        lbvh_nodes.data[pc.node_base + node].first_shape = 0u;
        lbvh_nodes.data[pc.node_base + node].shape_count = 0u;
        lbvh_nodes.data[pc.node_base + node].first_child = pc.node_base + 1u + 2u * index;
        lbvh_nodes.data[pc.node_base + node].child_count = 2u;
    }
}

void main()
{
    int i = int(gl_GlobalInvocationID.x);
    if (i >= int(pc.count) - 1) return;

    WORDS[pc.counters + uint(i)] = 0u;
    if (i == 0) {
        // The root is not anyone's child.
        WORDS[pc.parents] = kNoParent;
        WORDS[pc.inner_nodes] = 0u;
        lbvh_nodes.data[pc.node_base].first_shape = 0u;
        lbvh_nodes.data[pc.node_base].shape_count = 0u;
        lbvh_nodes.data[pc.node_base].first_child = pc.node_base + 1u;
        lbvh_nodes.data[pc.node_base].child_count = 2u;
    }

    // Which way this node's range runs, and how far.
    int d = common_bits(i, i + 1) > common_bits(i, i - 1) ? 1 : -1;
    int min_shared = common_bits(i, i - d);
    int max_length = 2;
    while (common_bits(i, i + max_length * d) > min_shared) {
        max_length *= 2;
    }
    int length = 0;
    for (int t = max_length / 2; t >= 1; t /= 2) {
        if (common_bits(i, i + (length + t) * d) > min_shared) {
            length += t;
        }
    }
    int j = i + length * d;

    // Where the range splits: the last place sharing more than the whole range does.
    int node_shared = common_bits(i, j);
    int split = 0;
    int step = length;
    do {
        step = (step + 1) / 2;
        if (common_bits(i, i + (split + step) * d) > node_shared) {
            split += step;
        }
    } while (step > 1);
    int gamma = i + split * d + min(d, 0);

    uint left = 1u + 2u * uint(i);
    uint right = left + 1u;
    place_child(uint(i), left, min(i, j) == gamma, uint(gamma));
    place_child(uint(i), right, max(i, j) == gamma + 1, uint(gamma + 1));
}
)";

/// Sets all bounds. Each leaf walks up, and at each inner node the first child to arrive
/// stops, while the second computes the node from both.
inline constexpr string_view lbvh_bounds_src = R"(
layout(local_size_x = 64) in;

// Trees are far shallower than this. It only keeps a bad table from hanging the GPU.
const uint kMaxDepth = 256u;

void main()
{
    uint sorted = gl_GlobalInvocationID.x;
    if (sorted >= pc.count) return;

    uint item = WORDS[pc.values_in + sorted];
    uint node = WORDS[pc.leaf_nodes + sorted];
    lbvh_nodes.data[pc.node_base + node].aabb_min = vec4(item_min(item), 0.0);
    lbvh_nodes.data[pc.node_base + node].aabb_max = vec4(item_max(item), 0.0);
    memoryBarrierBuffer();

    uint inner = WORDS[pc.parents + node];
    for (uint depth = 0u; depth < kMaxDepth && inner != kNoParent; ++depth) {
        if (atomicAdd(WORDS[pc.counters + inner], 1u) == 0u) return;
        memoryBarrierBuffer();

        BvhNode a = lbvh_nodes.data[pc.node_base + 1u + 2u * inner];
        BvhNode b = lbvh_nodes.data[pc.node_base + 2u + 2u * inner];
        uint at = WORDS[pc.inner_nodes + inner];
        lbvh_nodes.data[pc.node_base + at].aabb_min = vec4(min(a.aabb_min.xyz, b.aabb_min.xyz), 0.0);
        lbvh_nodes.data[pc.node_base + at].aabb_max = vec4(max(a.aabb_max.xyz, b.aabb_max.xyz), 0.0);
        memoryBarrierBuffer();

        inner = WORDS[pc.parents + at];
    }
}
)";

} // namespace velk::impl

#endif // VELK_SCENE_LBVH_H
