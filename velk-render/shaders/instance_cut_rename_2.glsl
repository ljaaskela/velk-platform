// `word` named again, or VELK_CUT_STALE when the pairs do not have it.
uint velk_rename(uint word)
{
    uint lo = 0u;
    uint hi = pc.pair_count;
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        uint old = cull_words.data[pc.pairs_word + mid * 2u];
        if (old == word) return cull_words.data[pc.pairs_word + mid * 2u + 1u];
        if (old < word) lo = mid + 1u;
        else hi = mid;
    }
    return VELK_CUT_STALE;
}

void main()
{
    uint cut = pc.cut_word;
    uint place = gl_GlobalInvocationID.x;
    if (place >= cull_words.data[cut + VELK_CUT_CAPACITY]) return;
    // Only the records in use: the page's partition holds this many from its place.
    uint header = cull_words.data[cut + VELK_CUT_PAGE_HEADERS] + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS;
    uint owner = cull_words.data[header + VELK_CUT_PAGE_OWNER];
    if (owner >= cull_words.data[cut + VELK_CUT_PARTITIONS]) return;
    uint at = cull_words.data[header + VELK_CUT_PAGE_PLACE] * VELK_CUT_PAGE_RECORDS + place % VELK_CUT_PAGE_RECORDS;
    if (at >= cull_words.data[velk_cut_state(cut, owner) + VELK_PART_COUNT]) return;
    uint record = velk_cut_record_word(cut, place);
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (entry == VELK_CUT_QUAD || entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
    bool stale = false;
    if (entry == 0xFFFFFFFFu) {
        uint table = cull_words.data[record + VELK_RECORD_MESH_TABLE];
        if (table != 0xFFFFFFFFu) {
            uint renamed = velk_rename(table);
            stale = renamed == VELK_CUT_STALE;
            cull_words.data[record + VELK_RECORD_MESH_TABLE] = renamed;
        }
    } else {
        uint renamed = velk_rename(entry & ~VELK_CUT_FOR_RAYS);
        stale = renamed == VELK_CUT_STALE;
        cull_words.data[record + VELK_RECORD_ENTRY] = stale ? VELK_CUT_STALE : renamed | (entry & VELK_CUT_FOR_RAYS);
    }
    uint item = cull_words.data[record + VELK_RECORD_SLOT];
    if (item < pc.item_count) {
        uint renamed = cull_words.data[pc.items_word + item];
        stale = stale || renamed == 0xFFFFFFFFu;
        cull_words.data[record + VELK_RECORD_SLOT] = renamed;
    } else if (item != 0xFFFFFFFFu) {
        // An instanced visual's item: where its block went, or stale with its visual gone.
        uint renamed = 0xFFFFFFFFu;
        for (uint b = 0u; b < pc.block_count && b < 4096u; ++b) {
            uint w = pc.blocks_word + b * 3u;
            uint first = cull_words.data[w];
            if (item >= first && item - first < cull_words.data[w + 1u]) {
                renamed = cull_words.data[w + 2u] + item - first;
                break;
            }
        }
        stale = stale || renamed == 0xFFFFFFFFu;
        cull_words.data[record + VELK_RECORD_SLOT] = renamed;
    }
    // What does not carry over: its partition chooses again (instance_cut_stale_src).
    if (stale) cull_words.data[record + VELK_RECORD_ENTRY] = VELK_CUT_STALE;
}
