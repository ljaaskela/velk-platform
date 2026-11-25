uint g_instance;
uint g_table;
uint g_traced = 0xFFFFFFFFu;  // what came of the traced record this thread draws, or 0xFFFFFFFF

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (g_traced != 0xFFFFFFFFu) g_traced = at >= pc.chosen_capacity ? 4u : 0u;
    if (at >= pc.chosen_capacity) return;
    uint record = pc.chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

// Lists a cluster for the second test, false when there is no room for it. Levels of detail
// were chosen already, so where from no longer matters: the camera stands in.
bool velk_retest(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[pc.retest_word + VELK_RETEST_LIST_COUNT], 1u);
    if (at >= pc.retest_capacity) return false;
    uint record = pc.retest_word + VELK_RETEST_LIST_RECORDS + at * VELK_RETEST_END;
    cull_words.data[record + VELK_RETEST_INSTANCE] = g_instance;
    cull_words.data[record + VELK_RETEST_SLOT] = slot;
    cull_words.data[record + VELK_RETEST_TABLE] = g_table;
    cull_words.data[record + VELK_RETEST_ENTRY] = entry;
    cull_words.data[record + VELK_RETEST_CLUSTER] = cluster;
    cull_words.data[record + VELK_RETEST_CUT_RECORD] = cut_record;
    cull_words.data[record + VELK_RETEST_CHOSEN_WORD] = pc.chosen_word;
    cull_words.data[record + VELK_RETEST_CHOSEN_CAPACITY] = pc.chosen_capacity;
    return true;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
}
