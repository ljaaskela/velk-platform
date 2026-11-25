#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 16, std430) buffer PyramidWords { uint data[]; } pyramid_words;

layout(push_constant) uniform PC {
    uint depth_id;       // the depth texture, when source_word is 0xFFFFFFFF
    uint source_word;
    uint source_width;
    uint source_height;
    uint target_word;
    uint width;
    uint height;
    uint compute_word;   // compute raster's depth plane, a word a pixel, or 0xFFFFFFFF for none
} pc;

float read_source(ivec2 at)
{
    if (pc.source_word == 0xFFFFFFFFu) {
        float depth = texelFetch(velk_textures[nonuniformEXT(pc.depth_id)], at, 0).r;
        if (pc.compute_word != 0xFFFFFFFFu) {
            // Its lowest bits break ties, not part of the depth.
            uint drawn = pyramid_words.data[pc.compute_word + uint(at.y) * pc.source_width + uint(at.x)];
            depth = max(depth, uintBitsToFloat(drawn & ~0xFFu));
        }
        return depth;
    }
    return uintBitsToFloat(pyramid_words.data[pc.source_word + uint(at.y) * pc.source_width + uint(at.x)]);
}

void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= pc.width || id.y >= pc.height) return;

    // An odd source's last column or row belongs to the last texel, so that none is left out.
    ivec2 last = ivec2(1 + ((pc.source_width & 1u) != 0u && id.x == pc.width - 1u ? 1 : 0),
                       1 + ((pc.source_height & 1u) != 0u && id.y == pc.height - 1u ? 1 : 0));
    ivec2 limit = ivec2(pc.source_width, pc.source_height) - 1;
    float farthest = 1e30;
    for (int y = 0; y <= last.y; ++y) {
        for (int x = 0; x <= last.x; ++x) {
            farthest = min(farthest, read_source(min(ivec2(id) * 2 + ivec2(x, y), limit)));
        }
    }
    pyramid_words.data[pc.target_word + id.y * pc.width + id.x] = floatBitsToUint(farthest);
}
