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
    uint descriptors_word;
    uint descriptor_count;
    uint items_word;
    uint first_item;
    uint item_count;
    uint objects_word;  // each item's object id, by item
    uint next_word;     // each item's next item of the same object, by item
} pc;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.item_count) return;
    uint item = pc.first_item + i;
    uint w = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;

    // Its visual: the descriptor whose items it is among.
    uint found = 0xFFFFFFFFu;
    uint at = pc.descriptors_word;
    for (uint k = 0u; k < pc.descriptor_count && k < 4096u; ++k) {
        uint first = cull_words.data[at + VELK_ITEM_DESCRIPTOR_FIRST];
        uint slots = cull_words.data[at + VELK_ITEM_DESCRIPTOR_SLOTS];
        if (item >= first && item - first < slots) {
            found = at;
            break;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + VELK_ITEM_DESCRIPTOR_MESHES], 4096u) * VELK_ITEM_MESH_WORDS;
    }
    // What does not change with its place: its table, how far it can move, its object, and no
    // next item, since an instanced object has one item.
    uint slot = found != 0xFFFFFFFFu ? item - cull_words.data[found + VELK_ITEM_DESCRIPTOR_FIRST] : 0u;
    cull_words.data[w + VELK_PLAIN_ITEM_TABLE] = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_TABLE] : 0xFFFFFFFFu;
    cull_words.data[w + VELK_PLAIN_ITEM_MOTION] = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_MOTION] : 0u;
    cull_words.data[w + VELK_PLAIN_ITEM_REST] = 0xFFFFFFFFu;
    uint object_base = found != 0xFFFFFFFFu ? cull_words.data[found + VELK_ITEM_DESCRIPTOR_OBJECT_BASE] : 0xFFFFFFFFu;
    cull_words.data[pc.objects_word + item] = object_base != 0xFFFFFFFFu ? object_base + slot : 0xFFFFFFFFu;
    cull_words.data[pc.next_word + item] = 0xFFFFFFFFu;
    uint place = 0xFFFFFFFFu;
    if (found != 0xFFFFFFFFu && cull_words.data[found + VELK_ITEM_DESCRIPTOR_SLOT_TABLE] != 0u) {
        place = cull_words.data[cull_words.data[found + VELK_ITEM_DESCRIPTOR_SLOT_TABLE] + slot];
    }
    if (place == 0xFFFFFFFFu) {
        cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_SLOT] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_DRAW] = 0xFFFFFFFFu;
        cull_words.data[w + VELK_PLAIN_ITEM_KIND] = 255u;
        cull_words.data[w + VELK_PLAIN_ITEM_MESH] = 0xFFFFFFFFu;
        return;
    }
    uint instance = cull_words.data[found + VELK_ITEM_DESCRIPTOR_INSTANCES] + place;
    cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE] = instance;
    cull_words.data[w + VELK_PLAIN_ITEM_SLOT] = place;
    // Its resting transform, three rows of four floats by raster place, as the motion pass reads it.
    uint rest = cull_words.data[found + VELK_ITEM_DESCRIPTOR_REST];
    if (rest != 0xFFFFFFFFu) cull_words.data[w + VELK_PLAIN_ITEM_REST] = rest + place * 12u;
    uint mesh = cull_instances.data[instance].params.y;
    if (mesh < cull_words.data[found + VELK_ITEM_DESCRIPTOR_MESHES]) {
        uint m = found + VELK_ITEM_DESCRIPTOR_HEADER_WORDS + mesh * VELK_ITEM_MESH_WORDS;
        cull_words.data[w + VELK_PLAIN_ITEM_DRAW] = cull_words.data[m + VELK_ITEM_MESH_DRAW];
        cull_words.data[w + VELK_PLAIN_ITEM_KIND] = cull_words.data[m + VELK_ITEM_MESH_KIND];
        cull_words.data[w + VELK_PLAIN_ITEM_MESH] = cull_words.data[m + VELK_ITEM_MESH_MESH];
    }
}
