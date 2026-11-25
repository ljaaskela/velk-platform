uint velk_cut_state(uint cut, uint index)
{
    return cull_words.data[cut + VELK_CUT_STATES_WORD] + index * VELK_CUT_PARTITION_WORDS;
}

// The word of the current page table, or the last frame's.
uint velk_cut_table(uint cut, bool current)
{
    uint parity = cull_words.data[cut + VELK_CUT_TABLE_PARITY];
    return cull_words.data[cut + VELK_CUT_TABLES + (current ? parity : 1u - parity)];
}

// The pool place of place `i` of a partition whose page table starts at word `table`: of its
// records, or of its ray list's entries.
uint velk_cut_place(uint table, uint i)
{
    return cull_words.data[table + i / VELK_CUT_PAGE_RECORDS] * VELK_CUT_PAGE_RECORDS + i % VELK_CUT_PAGE_RECORDS;
}

// The word of the record at pool place `place`.
uint velk_cut_record_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_RECORD_POOL] + place * VELK_RECORD_END;
}

// The word of the ray list entry at pool place `place`.
uint velk_cut_ray_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_RAY_POOL] + place * VELK_CUT_RAY_WORDS;
}

// The pool place of node `i` of the tree of a partition whose page table starts at word `table`.
uint velk_cut_node_place(uint table, uint i)
{
    return cull_words.data[table + i / VELK_CUT_PAGE_NODES] * VELK_CUT_PAGE_NODES + i % VELK_CUT_PAGE_NODES;
}

// The word of the tree node at pool place `place`.
uint velk_cut_node_word(uint cut, uint place)
{
    return cull_words.data[cut + VELK_CUT_NODE_POOL] + place * VELK_CUT_NODE_WORDS;
}

// The pages `records` records take.
uint velk_cut_pages(uint records)
{
    return (records + VELK_CUT_PAGE_RECORDS - 1u) / VELK_CUT_PAGE_RECORDS;
}

// Nodes in the tree over a partition's `records` records: one less than its leaves of four, at
// least one. Mirrors cut_tree_nodes.
uint velk_cut_tree_nodes(uint records)
{
    if (records == 0u) return 0u;
    uint leaves = (records + 3u) / 4u;
    return leaves > 1u ? leaves - 1u : 1u;
}
