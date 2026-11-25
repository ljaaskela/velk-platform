// Keeps partition `index`'s records, or lists it to choose again. `away` is from the camera
// to its bounds; `look` which way the camera looks. A partition that chose some clusters at
// the light's level only because they were out of view chooses again once the camera has
// turned by about 20 degrees, so that what comes into view gets its detail.
void velk_cut_decide(uint index, uint entry, uint chunks, vec3 cam, vec3 away, vec3 look)
{
    uint cut = pc.cut_word;
    uint state = velk_cut_state(cut, index);
    cull_words.data[state + VELK_PART_CHOOSING] = 0u;
    if (chunks == 0u) {
        cull_words.data[state + VELK_PART_COUNT] = 0u;
        cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        return;
    }
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));
    uint count = cull_words.data[state + VELK_PART_COUNT];
    bool same = cull_words.data[state + VELK_PART_CHUNK_FIRST] == cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK]
             && cull_words.data[state + VELK_PART_CHUNKS] == chunks
             && cull_words.data[state + VELK_PART_QUAD_TREE] == cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
    vec3 looked = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED]), uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED + 1u]),
                       uintBitsToFloat(cull_words.data[state + VELK_PART_LOOKED + 2u]));
    bool turned = cull_words.data[state + VELK_PART_OUT_OF_VIEW] != 0u && dot(looked, look) < 0.94;
    bool retry = cull_words.data[state + VELK_PART_RETRY] != 0u;
    cull_words.data[state + VELK_PART_RETRY] = 0u;
    if (same && count != 0u && !turned && !retry && length(cam - viewpoint) <= pc.refresh * length(away)) {
        // Kept: its pages stay, its page table goes into the current one.
        uint pages = velk_cut_pages(count);
        uint first = atomicAdd(cull_words.data[cut + VELK_CUT_TABLE_USED], pages);
        uint was = cull_words.data[state + VELK_PART_TABLE_FIRST];
        if (first + pages <= cull_words.data[cut + VELK_CUT_PAGES] && was + pages <= cull_words.data[cut + VELK_CUT_PAGES]) {
            uint from = velk_cut_table(cut, false) + was;
            uint to = velk_cut_table(cut, true) + first;
            for (uint p = 0u; p < pages; ++p) cull_words.data[to + p] = cull_words.data[from + p];
            cull_words.data[state + VELK_PART_TABLE_FIRST] = first;
            cull_words.data[state + VELK_PART_OLD_COUNT] = 0u;
            atomicAdd(cull_words.data[cut + VELK_CUT_KEPT_PAGES], pages);
            return;
        }
        // Never so, since a page is in one table at most: nothing for it this frame.
        cull_words.data[state + VELK_PART_COUNT] = 0u;
        cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
        return;
    }
    // Choosing again, its new records take over the light of its old ones that are the same
    // (entry, cluster and slot), which stay on its old pages: also when its members changed, as
    // when one of them moved, so that what stayed keeps its light. Its pages and ray list as
    // they were, kept for the frame should no free pages be there for its new ones.
    cull_words.data[state + VELK_PART_LAST_TABLE_FIRST] = cull_words.data[state + VELK_PART_TABLE_FIRST];
    // When it chose: the clusters whose errors change after it have it choose again.
    cull_words.data[state + VELK_PART_CHOSE] = velk_globals.data[pc.globals_base].present_counter + 1u;
    cull_words.data[state + VELK_PART_OLD_COUNT] = count;
    cull_words.data[state + VELK_PART_OLD_RAY_COUNT] = count != 0u ? cull_words.data[state + VELK_PART_RAY_COUNT] : 0u;
    cull_words.data[state + VELK_PART_SAME] = same ? 1u : 0u;
    cull_words.data[state + VELK_PART_FROM] = floatBitsToUint(cam.x);
    cull_words.data[state + VELK_PART_FROM + 1u] = floatBitsToUint(cam.y);
    cull_words.data[state + VELK_PART_FROM + 2u] = floatBitsToUint(cam.z);
    cull_words.data[state + VELK_PART_LOOKED] = floatBitsToUint(look.x);
    cull_words.data[state + VELK_PART_LOOKED + 1u] = floatBitsToUint(look.y);
    cull_words.data[state + VELK_PART_LOOKED + 2u] = floatBitsToUint(look.z);
    cull_words.data[state + VELK_PART_OUT_OF_VIEW] = 0u;
    cull_words.data[state + VELK_PART_CHUNK_FIRST] = cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK];
    cull_words.data[state + VELK_PART_CHUNKS] = chunks;
    cull_words.data[state + VELK_PART_QUAD_TREE] = cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
    cull_words.data[state + VELK_PART_COUNT] = 0u;
    // Nothing for rays until its tree pass lists its new records, or the place pass gives its
    // old ones back when no free pages are there for the new.
    cull_words.data[state + VELK_PART_RAY_COUNT] = 0u;
    cull_words.data[state + VELK_PART_CHOOSING] = 1u;
    atomicAdd(cull_words.data[cut + VELK_CUT_CHOSE_AGAIN], 1u);
    uint at = atomicAdd(cull_words.data[cut + VELK_CUT_CHOOSE_GROUPS], chunks);
    cull_words.data[state + VELK_PART_CHUNK_START] = at;
    uint list = cull_words.data[cut + VELK_CUT_CHOOSE_WORD];
    uint first_chunk = cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK];
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[list + (at + c) * VELK_CHUNK_END + VELK_CHUNK_FIRST_SLOT] = first_chunk + c * 64u;
        cull_words.data[list + (at + c) * VELK_CHUNK_END + VELK_CHUNK_PARTITION] = index;
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.entry_count) return;

    uint entry = pc.table_word + index * VELK_CULL_ENTRY_WORDS;
    uint chunks = cull_words.data[entry + VELK_PARTITION_ENTRY_CHUNKS];

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN + 1u]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MIN + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX + 1u]),
                   uintBitsToFloat(cull_words.data[entry + VELK_PARTITION_ENTRY_MAX + 2u]));

    GlobalData globals = velk_globals.data[pc.globals_base];
    lo -= globals.cam_pos.xyz;
    hi -= globals.cam_pos.xyz;
    vec3 away = max(max(lo, -hi), vec3(0.0));
    mat4 vp = globals.view_projection;
    vec3 look = normalize(vec3(vp[0][3], vp[1][3], vp[2][3]));

    velk_cut_decide(index, entry, chunks, globals.cam_pos.xyz, away, look);
}
