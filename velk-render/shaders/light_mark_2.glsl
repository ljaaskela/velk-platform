// Lists the one value at `at`, of kind `kind` (VELK_LIGHT_TRACE_*), when a probe's ray asked for
// it and it is due, as a later trace: of the rays the last frame's first traces left, at most
// half the frame's, and at most half the trace list, so that the claim's first traces after it
// always have room. Its frame goes into it, so that nothing lists it again this frame; when it
// finds no room it is given back, so that a pixel's claim can still list it, and it is listed
// again on a later frame.
void velk_light_list_asked(uint at, uint kind, uint target, uint instance, uint table, uint frame)
{
    uint pool = pc.pool_word;
    uint value = light_words.data[at];
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING) return;
    if ((value & VELK_LIGHT_ONE_ASKED) == 0u || !velk_light_one_due(value)) return;
    uint listed = (value & ~(31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | ((frame & 31u) << VELK_LIGHT_ONE_QUEUED_SHIFT);
    if (listed == value || atomicCompSwap(light_words.data[at], value, listed) != value) return;
    uint rays = 8u * clamp(light_words.data[pool + VELK_POOL_LIGHTS], 1u, VELK_LIGHT_SLOTS);
    uint budget = light_words.data[pool + VELK_POOL_RAY_BUDGET];
    uint most = min(budget - min(light_words.data[pool + VELK_POOL_LAST_FIRST_RAYS_ASKED], budget), budget / 2u);
    bool room = light_words.data[pool + VELK_POOL_TRACES] < VELK_LIGHT_TRACE_CAPACITY / 2u;
    if (room) {
        uint before = atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], rays);
        if (before + rays > most) {
            atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], 0u - rays);
            room = false;
        }
    }
    if (room) {
        // Every place under the count is written, as velk_light_list_entry writes them.
        uint i = atomicAdd(light_words.data[pool + VELK_POOL_TRACES], 1u);
        if (i < VELK_LIGHT_TRACE_CAPACITY) {
            uint w = light_words.data[pool + VELK_POOL_TRACE_WORD] + i * VELK_LIGHT_TRACE_WORDS;
            light_words.data[w + VELK_TRACE_ENTRY_HOW] = kind;
            light_words.data[w + VELK_TRACE_ENTRY_AT] = target;
            light_words.data[w + VELK_TRACE_ENTRY_INSTANCE] = instance;
            light_words.data[w + VELK_TRACE_ENTRY_TABLE] = table;
            return;
        }
        atomicAdd(light_words.data[pool + VELK_POOL_RAYS_LISTED], 0u - rays);
    }
    atomicCompSwap(light_words.data[at], listed, value);
}

// Never past the pool's pages, whatever a record says.
void velk_light_mark(uint page)
{
    if (page >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
    atomicOr(light_words.data[light_words.data[pc.pool_word + VELK_POOL_MARKS_WORD] + (page >> 5u)], 1u << (page & 31u));
}

// A record's first page and its others. Its pieces are marked apart (light_pieces_src).
void velk_light_mark_record(uint light)
{
    velk_light_mark(light);
    uint first = velk_light_page(pc.pool_word, light);
    for (uint k = 1u; k < VELK_LIGHT_DIRECT_PAGES; ++k) velk_light_mark(light_words.data[first + VELK_PAGE_PAGES - 1u + k]);
}

void main()
{
    uint cut = pc.cut_word;
    // A record of the pool a thread, only those a partition holds.
    uint record;
    uint state;
    uint at;
    if (!velk_cut_live_record(cut, gl_GlobalInvocationID.x, record, state, at)) return;
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    GlobalData g = velk_globals.data[pc.globals_base];
    uint frame = g.present_counter;
    // A quad's light is on the node over it as wide as the light spacing, as the claim finds it.
    if (entry == VELK_CUT_QUAD) {
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(light_words.data[record + VELK_RECORD_QUAD_TREE], light_words.data[record + VELK_RECORD_QUAD_NODE],
                                         g.cam_pos.xyz, pixels_per_unit, VELK_LIGHT_PIXELS);
        velk_light_list_asked(node + VELK_QUAD_NODE_LIGHT, VELK_LIGHT_TRACE_NODE, node, 0u, 0u, frame);
        return;
    }
    uint light = light_words.data[record + VELK_RECORD_LIGHT];
    if (light == VELK_LIGHT_NONE || light == VELK_LIGHT_PENDING) return;
    // Inheriting (kLightInherit): the coarser record's pages it lays its own out from are kept.
    if (velk_light_inherits(light)) {
        uint parent = light & ~VELK_LIGHT_INHERIT;
        if (parent < light_words.data[pc.pool_word + VELK_POOL_PAGES]) velk_light_mark_record(parent);
        return;
    }
    if (light >= VELK_LIGHT_ONE_VALUE) {
        atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_ONE_VALUE], 1u);
        // Asked for by a probe's ray: traced as the claim traces it, from its cluster's sphere or
        // its whole mesh's. A record for rays only (kCutForRays) is its cluster's too.
        if ((light & VELK_LIGHT_ONE_ASKED) == 0u || entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
        if (velk_cut_for_rays(entry)) entry &= ~VELK_CUT_FOR_RAYS;
        uint slot = light_words.data[record + VELK_RECORD_SLOT];
        uint instance = pc.instances_base + slot;
        if (pc.items_word != VELK_LIGHT_NONE) instance = light_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
        if (instance == VELK_LIGHT_NONE) return;
        if (entry == 0xFFFFFFFFu) {
            uint table = light_words.data[record + VELK_RECORD_MESH_TABLE];
            velk_light_list_asked(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_MESH, record, instance, table, frame);
        } else {
            uint cluster = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
            velk_light_list_asked(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_CLUSTER, record, instance, cluster, frame);
        }
        return;
    }
    uint pages = light_words.data[pc.pool_word + VELK_POOL_PAGES];
    if (light >= pages) return;
    atomicAdd(light_words.data[pc.pool_word + VELK_POOL_RECORDS_PAGED], 1u);
    velk_light_mark_record(light);
    light_words.data[velk_light_page(pc.pool_word, light) + VELK_PAGE_READ] = 0u;
}
