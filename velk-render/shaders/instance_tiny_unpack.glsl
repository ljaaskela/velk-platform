#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 16, std430) buffer TinyWords { uint data[]; } tiny_words;
layout(set = 1, binding = 16, std430) buffer TinyWide { uint64_t data[]; } tiny_wide;

layout(push_constant) uniform PC {
    uint list_word;
    uint planes_word;
    uint width;
    uint height;
    uint wide_word;
} pc;

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    uint pixel = coord.y * pc.width + coord.x;
    uint64_t packed = tiny_wide.data[pc.wide_word / 2u + pixel];
    if (packed == 0ul) return;
    tiny_wide.data[pc.wide_word / 2u + pixel] = 0ul;
    uint low = uint(packed & 0xFFFFFFFFul);
    uint index = low >> 8u;
    uint t = low & 0xFFu;
    if (index >= tiny_words.data[pc.list_word + VELK_TINY_LIST_DRAWN]) return;
    uint record = pc.list_word + VELK_TINY_LIST_RECORDS + index * VELK_TINY_END;
    tiny_words.data[pc.planes_word + pixel] = uint(packed >> 32);
    uint ids = pc.planes_word + pc.width * pc.height + pixel * VELK_PIXEL_RECORD_WORDS;
    tiny_words.data[ids] = (tiny_words.data[record + VELK_TINY_DRAW] + 1u) | VELK_IDS_TINY | (t << VELK_IDS_TRIANGLE_SHIFT);
    tiny_words.data[ids + 1u] = record;
}
