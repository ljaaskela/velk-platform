// The long lists' words of the cut (VELK_CUT_LARGE): its header, then where its queue, block
// table and blocks' words (VELK_CUT_LARGE_BLOCK, then digit counts) are. velk_large_words sets
// them from the shader's cut_word.
uint g_large = 0u;
uint g_table_word = 0u;
uint g_counts_word = 0u;

void velk_large_words()
{
    g_large = cull_words.data[pc.cut_word + VELK_CUT_LARGE_WORD];
    g_table_word = g_large + VELK_CUT_LARGE_END + cull_words.data[pc.cut_word + VELK_CUT_PARTITIONS] * VELK_CUT_LARGE_LIST_END;
    g_counts_word = g_table_word + cull_words.data[g_large + VELK_CUT_LARGE_CAP];
}

// The word of queue place `slot`.
uint velk_large_list_word(uint slot)
{
    return g_large + VELK_CUT_LARGE_END + slot * VELK_CUT_LARGE_LIST_END;
}

// The queue place of the list this work group's block is of, and the block's first record in its
// list; false for a block of no list, or past the room.
bool velk_large_block_slot(out uint list, out uint first)
{
    list = 0u;
    first = 0u;
    uint block = gl_WorkGroupID.x;
    if (block >= min(cull_words.data[g_large + VELK_CUT_LARGE_BLOCKS], cull_words.data[g_large + VELK_CUT_LARGE_CAP])) return false;
    uint slot = cull_words.data[g_table_word + block];
    if (slot == 0xFFFFFFFFu) return false;
    list = velk_large_list_word(slot);
    first = (block - cull_words.data[list + VELK_CUT_LARGE_LIST_FIRST_BLOCK]) * VELK_CUT_LARGE_BLOCK_ENTRIES;
    return true;
}
