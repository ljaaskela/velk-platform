#version 450
#include "velk.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint cut_word;
} pc;

void main()
{
    uint cut = pc.cut_word;
    cull_words.data[cut + VELK_CUT_TABLE_PARITY] = 1u - cull_words.data[cut + VELK_CUT_TABLE_PARITY];
    cull_words.data[cut + VELK_CUT_TABLE_USED] = 0u;
    cull_words.data[cut + VELK_CUT_PAGES_TAKEN] = 0u;
    cull_words.data[cut + VELK_CUT_KEPT_PAGES] = 0u;
    cull_words.data[cut + VELK_CUT_PAGES_DEMAND] = 0u;
    // A new frame of the takeover table: what it held is gone.
    cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME] += 1u;
    cull_words.data[cut + VELK_CUT_LAST_FOR_RAYS_RECORDS] = cull_words.data[cut + VELK_CUT_FOR_RAYS_RECORDS];
    cull_words.data[cut + VELK_CUT_FOR_RAYS_RECORDS] = 0u;
    cull_words.data[cut + VELK_CUT_LAST_REWRITTEN] = cull_words.data[cut + VELK_CUT_REWRITTEN];
    cull_words.data[cut + VELK_CUT_LAST_TOOK_OVER] = cull_words.data[cut + VELK_CUT_TOOK_OVER];
    cull_words.data[cut + VELK_CUT_REWRITTEN] = 0u;
    cull_words.data[cut + VELK_CUT_TOOK_OVER] = 0u;
    cull_words.data[cut + VELK_CUT_LAST_LONGEST_WALK] = cull_words.data[cut + VELK_CUT_LONGEST_WALK];
    cull_words.data[cut + VELK_CUT_LAST_MOST_CHOSEN] = cull_words.data[cut + VELK_CUT_MOST_CHOSEN];
    cull_words.data[cut + VELK_CUT_LONGEST_WALK] = 0u;
    cull_words.data[cut + VELK_CUT_MOST_CHOSEN] = 0u;
    cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS] = 0u;
    cull_words.data[cut + VELK_CUT_VISIBLE_GROUPS] = 0u;
}
