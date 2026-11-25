bool velk_tiny_add(uint draw, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.tiny_word + VELK_TINY_LIST_COUNT], 1u);
    if (at >= pc.tiny_capacity) return false;
    uint record = pc.tiny_word + VELK_TINY_LIST_RECORDS + at * VELK_TINY_END;
    cull_words.data[record + VELK_TINY_DRAW] = draw;
    cull_words.data[record + VELK_TINY_CLUSTER] = cluster;
    cull_words.data[record + VELK_TINY_SLOT] = slot;
    cull_words.data[record + VELK_TINY_CUT_RECORD] = cut_record;
    return true;
}
