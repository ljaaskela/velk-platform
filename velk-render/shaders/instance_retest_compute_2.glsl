uint g_chosen_word;
uint g_chosen_capacity;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[g_chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (at >= g_chosen_capacity) return;
    uint record = g_chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming);
