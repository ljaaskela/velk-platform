void main()
{
    uint cut = pc.cut_word;
    uint index = gl_GlobalInvocationID.x;
    uint pages = cull_words.data[cut + VELK_CUT_PAGES];
    uint marks = cull_words.data[cut + VELK_CUT_PAGE_MARKS];
    if (pc.phase == 0u) {
        if (index == 0u) {
            // Whether the pool is too small, once every partition choosing again has taken its
            // pages or failed to: when the pages kept and asked for do not fit it, or when none
            // could be placed while some asked, with free pages fewer than even the smallest ask.
            // A shortage that partitions keeping their old records for a frame drain is neither.
            uint kept = cull_words.data[cut + VELK_CUT_KEPT_PAGES];
            uint demand = cull_words.data[cut + VELK_CUT_PAGES_DEMAND];
            uint free_now = min(cull_words.data[cut + VELK_CUT_FREE_PAGES], pages);
            // The pages asked for and not got, which the CPU watches every frame while there are
            // any, so that a pool too small grows soon.
            uint taken = cull_words.data[cut + VELK_CUT_PAGES_TAKEN];
            cull_words.data[cut + VELK_CUT_SHORT_PAGES] = demand > taken ? demand - taken : 0u;
            if (kept + demand > pages) {
                atomicMax(cull_words.data[cut + VELK_CUT_PAGES_WANTED], kept + demand);
            } else if (demand != 0u && cull_words.data[cut + VELK_CUT_PAGES_TAKEN] == 0u) {
                // Every ask was more than the free pages, so this is more than the pool.
                atomicMax(cull_words.data[cut + VELK_CUT_PAGES_WANTED], pages - free_now + demand);
            }
            cull_words.data[cut + VELK_CUT_FREE_PAGES] = 0u;
            cull_words.data[cut + VELK_CUT_DRAW_GROUPS] = 0u;
        }
        if (index >= min(cull_words.data[cut + VELK_CUT_TABLE_USED], pages)) return;
        uint page = cull_words.data[velk_cut_table(cut, true) + index];
        if (page < pages) atomicOr(cull_words.data[marks + page / 32u], 1u << (page % 32u));
        return;
    }
    if (index * 32u >= pages) return;
    uint used = cull_words.data[marks + index];
    uint free_pages = 0u;
    uint last = 0u;
    for (uint b = 0u; b < 32u && index * 32u + b < pages; ++b) {
        if ((used & (1u << b)) == 0u) ++free_pages;
        else last = index * 32u + b + 1u;
    }
    if (last != 0u) atomicMax(cull_words.data[cut + VELK_CUT_DRAW_GROUPS], last);
    uint at = cull_words.data[cut + VELK_CUT_STACK_WORD] + atomicAdd(cull_words.data[cut + VELK_CUT_FREE_PAGES], free_pages);
    uint headers = cull_words.data[cut + VELK_CUT_PAGE_HEADERS];
    for (uint b = 0u; b < 32u && index * 32u + b < pages; ++b) {
        if ((used & (1u << b)) != 0u) continue;
        uint page = index * 32u + b;
        cull_words.data[at++] = page;
        cull_words.data[headers + page * VELK_CUT_PAGE_WORDS + VELK_CUT_PAGE_OWNER] = 0xFFFFFFFFu;
    }
    cull_words.data[marks + index] = 0u;
}
