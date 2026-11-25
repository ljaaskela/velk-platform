#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;

layout(push_constant) uniform PC {
    uint list_word;
    uint capacity;
} pc;

void main()
{
    uint count = min(tiny_words.data[pc.list_word + VELK_TINY_LIST_COUNT], pc.capacity);
    uint row = clamp(count, 1u, 65535u);
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS] = count == 0u ? 0u : row;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS + 1u] = (count + row - 1u) / row;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_GROUPS + 2u] = 1u;
    tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN] = count;
}
