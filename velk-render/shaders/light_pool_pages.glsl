// The first word of page `page` of the pool at `pool`.
uint velk_light_page(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[pool + VELK_POOL_SEGMENT_WORDS + (page >> 14u)] + (page & 16383u) * VELK_LIGHT_PAGE_WORDS;
}

// The record at place `place` of the kept cut `cut`'s pool, when a partition holds it: its
// word, the partition's state and the record's place in the partition's page table. False for a
// place of a free page, or past the records its page's partition holds.
bool velk_cut_live_record(uint cut, uint place, out uint record, out uint state, out uint at)
{
    record = 0u;
    state = 0u;
    at = 0u;
    if (place >= VELK_LIGHT_WORDS.data[cut + VELK_CUT_CAPACITY]) return false;
    uint header = VELK_LIGHT_WORDS.data[cut + VELK_CUT_PAGE_HEADERS] + (place / VELK_CUT_PAGE_RECORDS) * VELK_CUT_PAGE_WORDS;
    uint owner = VELK_LIGHT_WORDS.data[header + VELK_CUT_PAGE_OWNER];
    if (owner >= VELK_LIGHT_WORDS.data[cut + VELK_CUT_PARTITIONS]) return false;
    state = VELK_LIGHT_WORDS.data[cut + VELK_CUT_STATES_WORD] + owner * VELK_CUT_PARTITION_WORDS;
    at = VELK_LIGHT_WORDS.data[header + VELK_CUT_PAGE_PLACE] * VELK_CUT_PAGE_RECORDS + place % VELK_CUT_PAGE_RECORDS;
    if (at >= VELK_LIGHT_WORDS.data[state + VELK_PART_COUNT]) return false;
    record = VELK_LIGHT_WORDS.data[cut + VELK_CUT_RECORD_POOL] + place * VELK_RECORD_END;
    return true;
}

// Words before the points of a record with `table` words after its header: an even number, so
// that a point's two words are on the same page.
uint velk_light_skip_for(uint table)
{
    return VELK_PAGE_ROOTS + ((table + 1u) & ~1u);
}

// Whether the record whose first page starts at `first` has its own points in pieces, and if so
// how many triangles; 0 for none.
uint velk_light_pieced(uint first)
{
    uint kind = VELK_LIGHT_WORDS.data[first + VELK_PAGE_KIND];
    return (kind & VELK_LIGHT_PIECED) != 0u ? min(kind & 0xFFFFu, 256u) : 0u;
}

// Words before the points of the record whose first page starts at `first`: its header, and for
// own points in pieces (VELK_LIGHT_PIECED) a word per triangle naming its root piece.
uint velk_light_skip(uint first)
{
    return velk_light_skip_for(velk_light_pieced(first));
}

// Page `k` of the record whose first page starts at `first`, k from 1, as its header names it.
uint velk_light_nth_page(uint first, uint k)
{
    return VELK_LIGHT_WORDS.data[first + VELK_PAGE_PAGES - 1u + min(k, VELK_LIGHT_DIRECT_PAGES - 1u)];
}

// The arena word of word `at` of the record whose first page starts at `first`, counted from
// its first page's first word across its pages.
uint velk_light_word(uint pool, uint first, uint at)
{
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    if (k == 0u) return first + at;
    return velk_light_page(pool, velk_light_nth_page(first, k)) + at % VELK_LIGHT_PAGE_WORDS;
}

// Point word `p` of the record whose first page starts at word `first`.
uint velk_light_point_word(uint pool, uint first, uint p)
{
    return velk_light_word(pool, first, velk_light_skip(first) + p);
}

// The word of own point `p` of the record whose first page starts at `first`: how much of each
// slot's light it does not get. Its trace state is the word after it, on the same page.
uint velk_light_own_word(uint pool, uint first, uint p)
{
    return velk_light_word(pool, first, velk_light_skip(first) + 2u * p);
}

// The own points of the record whose first page starts at `first`, two words each.
uint velk_light_own_points(uint first)
{
    return VELK_LIGHT_WORDS.data[first + VELK_PAGE_POINT_WORDS] / 2u;
}

// The blocker word of the point whose two words start at word `at` of page `page`: for each
// slot, 4 bits of what its last ray to that light found (VELK_LIGHT_BLOCKER_*). Not in the pages: each segment has a word after its pages for each two words of
// them, so a point's is where its two words are, halved.
uint velk_light_page_blocker(uint pool, uint page, uint at)
{
    uint segment = VELK_LIGHT_WORDS.data[pool + VELK_POOL_SEGMENT_WORDS + (page >> 14u)];
    uint in_segment = (page & 16383u) * VELK_LIGHT_PAGE_WORDS + at;
    return segment + VELK_LIGHT_SEGMENT_PAGE_WORDS + in_segment / 2u;
}

// The blocker word of own point `p` of the record whose first page starts at `first`.
uint velk_light_blocker_word(uint pool, uint first, uint p)
{
    uint at = velk_light_skip(first) + 2u * p;
    uint k = at / VELK_LIGHT_PAGE_WORDS;
    uint page = k == 0u ? VELK_LIGHT_WORDS.data[first + VELK_PAGE_FIRST] : velk_light_nth_page(first, k);
    return velk_light_page_blocker(pool, page, at % VELK_LIGHT_PAGE_WORDS);
}
