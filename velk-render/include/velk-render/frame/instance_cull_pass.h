#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_PASS_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_PASS_H

#include <velk-render/frame/instance_cull_layout.h>
#include <velk-render/frame/instance_cull_shaders.h>

namespace velk {

/// The culling's passes, as the GPU timer names them: which partitions keep their records;
/// for those that do not, counting their records (with the copies for those that do), placing
/// them and writing them; the trees for rays, the light pages kept, and the draw lists.
inline constexpr uint32_t kCullStages = 7;
inline constexpr const char* kCullStageNames[kCullStages] = {
    "instance.decide", "instance.count", "instance.place", "instance.write",
    "instance.trees", "instance.light_marks", "instance.draws"};
/// The same for a view chosen as another camera.
inline constexpr const char* kCullOtherStageNames[kCullStages] = {
    "other:instance.decide", "other:instance.count", "other:instance.place", "other:instance.write",
    "other:instance.trees", "other:instance.light_marks", "other:instance.draws"};

/**
 * @brief One view's GPU culling passes, kept by the render path. Set @c dirty when the
 *        view's batches change.
 */
struct InstanceCullPass
{
    /// The culling, as passes timed apart, in order: kCullStageNames.
    IRenderPass::Ptr passes[kCullStages];
    bool dirty = true;
    /// The view is chosen as another camera: its passes are timed under kCullOtherStageNames.
    bool chosen_by_other = false;
    /// The GPU time the light trace aims for a frame (ICamera::light_trace_ms); 0 for no limit.
    float light_trace_ms = 4.f;
    vector<uint32_t> count_words;  ///< Reset before every cull.
    IGpuBuffer* arena_buffer = nullptr;
    size_t arena_size = 0;
    uint32_t arena_generation = 0;  ///< Changes when the arena's buffer is replaced, which draws naming it must follow.
    /// What reserve_instance_cull worked out this frame, for emit_instance_cull_pass to record:
    /// whether it reserved, whether the recording must be made again, whether the scene's
    /// objects have a kept cut, and the light pool's word.
    bool reserved = false;
    bool reserved_record = false;
    bool reserved_plain_cut = false;
    uint32_t reserved_light_pool_word = kCullNoSlot;
    uint64_t partition_tables = 0;  ///< Which tables the recording uses.
    /// The batches culled together, plain objects: see instance_cull_compute_src. Holds the
    /// clusters chosen, the entries, the items (kPlainItemWords: instance index, slot, table, draw; no table for one
    /// drawn by its own pass that rays see), each item's
    /// object id, each item's next item of the same object, by object slot its first item, and
    /// by item the word of the kept cut record of one drawn by its own pass, 0 for none, written
    /// each frame.
    ArenaRegion together;
    /// Each batch drawn by its own pass that rays see, its first item (in `word`) and its items.
    vector<CutDrawWords::OwnRecords> own_items;
    /// Each batch drawn by its own pass whose clusters (IBatch::get_ray_cull) the culling lays out.
    vector<const IBatch*> own_clusters;
    uint32_t together_items = 0;
    uint32_t together_entries = 0;
    uint32_t together_chosen = 0;  ///< How many records fit.
    uint32_t together_object_slots = 0;  ///< Object slots the first items are by.
    /// Items whose mesh had no ray data yet when they were made (kPlainItemWords): asked again
    /// each frame until it has.
    struct PendingMesh
    {
        IMeshPrimitive::Ptr primitive;
        uint32_t first_item = 0;
        uint32_t count = 0;
    };
    vector<PendingMesh> pending_meshes;
    /// The instanced visuals' items, the last of the items: where they start and how many, and
    /// their descriptors (kItemDescriptorHeaderWords then kItemMeshWords a mesh each), as last
    /// written, in their region, which instance_items_src reads. And each visual's slots when its
    /// items were made, by key: more slots make the items again.
    uint32_t instanced_first_item = 0;
    uint32_t instanced_items = 0;
    vector<uint32_t> item_descriptors;
    uint32_t item_descriptor_count = 0;
    ArenaRegion item_descriptor_region;
    vector<std::pair<const void*, uint32_t>> instanced_slots;
    /// Whether any of the items is moved by a motion function, which rays walking the cut are told.
    bool items_move = false;
    ArenaRegion retest;            ///< What failed the first occlusion test.
    uint32_t retest_capacity = 0;
    /// Where clusters with triangles too small on screen for the hardware go, set by the path
    /// before culling: see kTinyHeaderWords. kCullNoSlot for none.
    uint32_t tiny_word = kCullNoSlot;
    uint32_t tiny_capacity = 0;
    float tiny_pixels = 0.f;  ///< Clusters whose mean triangle is less than this across on screen go to compute.
    uint32_t recorded_tiny_word = kCullNoSlot;  ///< What the recordings use.
    float recorded_tiny_pixels = 0.f;
    bool recorded_whole_cut = false;
    uint32_t pyramid_word = kCullNoSlot;  ///< Which pyramid the recording reads.
    IRenderPass::Ptr retest_pass;
    bool retest_dirty = true;
    /// A partitioned visual's kept cut for this view: see kCutHeaderWords.
    struct Cut
    {
        const void* key = nullptr;  ///< The visual, as InstanceCullInfo::key.
        ArenaRegion region;
        uint32_t partitions = 0;
        uint32_t capacity = 0;      ///< Records the pool holds, kCutPageRecords a page.
        uint32_t node_capacity = 0; ///< Tree nodes the pool holds, kCutPageNodes a page.
        uint64_t made_frame = 0;    ///< The frame it was made, for watching its pool closely at first.
        bool short_pages = false;   ///< Partitions lately asked for pages they did not get.
        uint32_t large_word = 0;    ///< The word of its sort of long ray lists (kCutLargeHeaderWords).
    };
    vector<Cut> cuts;
    /// The view's quads drawn (kQuadListHeaderWords), filled each frame by the draw pass from
    /// the kept cuts' quad records, and how many it holds.
    ArenaRegion quads;
    uint32_t quad_capacity = 0;
    /// The top tree for rays over all the cuts' partitions (kRayTopHeaderWords), and what its
    /// cuts and leaves were written for.
    ArenaRegion ray_top;
    uint64_t ray_top_layout = 0;
    bool ray_top_fresh = false;  ///< Made this frame for a new layout, its words still to be written.
    /// The top tree's cuts' words, in its order (kRayTopCutWords), for passes over each cut.
    vector<uint32_t> ray_top_cuts;
    /// Visuals whose pools were too small, with the records they wanted: theirs are made
    /// again with room for those.
    vector<std::pair<const void*, uint32_t>> cut_growth;
    uint64_t cut_check_frame = 0;  ///< When the pools' needs were last read back.
    uint64_t cut_log_frame = 0;    ///< When the count of partitions choosing again was last logged.
    uint32_t cut_moves = 0;        ///< What the count was then.
    bool cut_moves_known = false;  ///< Whether that count is comparable with the next.
    /// The light of the kept cuts' records, which the culling frees each frame what no record
    /// names of, and the path gives records the first time it draws them (light_claim_src).
    LightPool light_pool;

    /// The trace of one pixel (kTraceHeaderWords): its region, made when the path asks for one by
    /// setting trace_x, which it is then written with, and when it was last logged.
    ArenaRegion trace;
    uint32_t trace_x = kTraceNone;
    uint32_t trace_y = 0;
    uint32_t trace_entry = kTraceNone;  ///< A cluster entry whose walks are noted wherever they go.
    uint64_t trace_log_frame = 0;

    /// What kept cut records name in the batches, as the batches were last made, so that a cut
    /// outlives batches made again (instance_cut_rename_src): each batch's tables, by their
    /// word, with each mesh's first cluster entry and how many, and the visual for one that has
    /// a kept cut of its own.
    struct BatchTables
    {
        const void* key = nullptr;
        uint32_t table_word = 0;
        vector<uint32_t> first_entry;
        vector<uint32_t> entry_count;
    };
    vector<BatchTables> batch_tables;
    /// The items as last made: whose object each is, which mesh (none for one drawn by its own
    /// pass, an instanced visual's key for its items), which of that object's items with that
    /// mesh, and its batch's tables.
    struct ItemName
    {
        uint32_t object = 0;
        const void* primitive = nullptr;
        uint32_t ordinal = 0;
        uint32_t table_word = 0;
    };
    vector<ItemName> item_names;
    /// Kept cuts whose records name the batches made again this frame, and what each old word
    /// and item becomes, for one frame.
    struct Rename
    {
        const void* key = nullptr;
        vector<uint32_t> pairs;  ///< Old word, new word, sorted by the old.
        vector<uint32_t> items;  ///< By old item, the new one: plain items only.
        /// The instanced visuals' blocks of items that carry over, three words each: the old
        /// first item, how many carry over and the new first item. An item past the plain items
        /// in none of them is of a visual that went.
        vector<uint32_t> blocks;
        uint32_t pairs_word = 0; ///< Where they are in rename_words.
        uint32_t items_word = 0;
        uint32_t blocks_word = 0;
    };
    vector<Rename> renames;
    ArenaRegion rename_words;
    bool renamed = false;  ///< The recording renames, so the next frame records without.
};

/// Words per mesh in a batch's culling table: VELK_BATCH_TABLE (cull_layout.h).
inline constexpr uint32_t kBatchTableWords = kTableEnd;

/**
 * Lays out the runs of draws by cluster and fills them, after a phase of culling.
 */
template <class Command>
inline void record_cluster_layout(Command& cmd, InstanceCullPass& state, IGpuPipeline* runs_pipeline,
                                  IGpuPipeline* fill_pipeline, IGpuBuffer* arena_buffer, uint32_t together_word,
                                  uint32_t together_entries_word, bool second_phase)
{
    const bool together = state.together_items != 0;
    if (!together) {
        return;
    }
    struct RunsPushC
    {
        uint32_t entry;
        uint32_t entries_word;
        uint32_t entry_count;
        uint32_t chosen_word;
        uint32_t second_phase;
    };
    const auto record_runs = [&](const RunsPushC& pc, uint32_t groups) {
        DispatchCall dc{};
        dc.pipeline = runs_pipeline;
        dc.groups_x = groups;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch(dc);
    };
    const uint32_t phase = second_phase ? 1u : 0u;
    record_runs({0u, together_entries_word, state.together_entries, together_word, phase}, (state.together_entries + 63u) / 64u);
    cmd.record_global_barrier();
    struct FillPushC
    {
        uint32_t chosen_word;
        uint32_t chosen_capacity;
    };
    const auto record_fill = [&](const FillPushC& pc) {
        DispatchCall dc{};
        dc.pipeline = fill_pipeline;
        dc.groups_x = 1;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd.record_dispatch_indirect(dc, arena_buffer, uint64_t(pc.chosen_word + kChosenListGroups) * sizeof(uint32_t));
    };
    record_fill({together_word, state.together_chosen});
    cmd.record_global_barrier();
}

/// What the kept cut of a view's plain objects is found by, among the visuals' (InstanceCullPass::Cut::key).
inline constexpr int kPlainCutKey = 0;

/// The word of @p state's kept cut of the visual @p key, or kCullNoSlot when it has none.
inline uint32_t instance_cut_word(const InstanceCullPass& state, const void* key)
{
    for (const auto& cut : state.cuts) {
        if (cut.key == key) {
            return static_cast<uint32_t>(cut.region.offset() / sizeof(uint32_t));
        }
    }
    return kCullNoSlot;
}

/// The word of @p state's quad list (kQuadListHeaderWords), or kCullNoSlot.
inline uint32_t instance_quad_word(const InstanceCullPass& state)
{
    return state.quads.valid() ? static_cast<uint32_t>(state.quads.offset() / sizeof(uint32_t)) : kCullNoSlot;
}

/// What the top tree over @p state's kept cuts holds, as a hash: its cut (the scene's objects',
/// when @p plain_cut, over @p plain_count partitions), where that cut's items are, and the quad
/// list. Its region is made again when this changes.
inline uint64_t instance_ray_top_layout(const InstanceCullPass& state, bool plain_cut, uint32_t plain_count)
{
    uint64_t layout = 0xcbf29ce484222325ULL;
    if (!plain_cut) {
        return layout;
    }
    uint32_t cut = kCullNoSlot;
    for (const auto& c : state.cuts) {
        if (c.key == &kPlainCutKey) {
            cut = static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
        }
    }
    if (cut == kCullNoSlot) {
        return layout;
    }
    const uint32_t together_word = state.together_items != 0 ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
    const uint32_t items_word = together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords
                              + state.together_entries;
    for (uint32_t v : {cut, 0u, items_word, plain_count, state.items_move ? 1u : 0u, instance_quad_word(state)}) {
        layout = (layout ^ v) * 0x100000001b3ULL;
    }
    return layout;
}

/// The word of @p state's records of the items drawn by their own pass (InstanceCullPass::together),
/// one an item, or kCullNoSlot.
inline uint32_t instance_item_records_word(const InstanceCullPass& state)
{
    if (!state.together.valid() || state.together_items == 0) {
        return kCullNoSlot;
    }
    return static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) + kCullChosenHeaderWords
         + state.together_chosen * kCullChosenRecordWords + state.together_entries
         + state.together_items * (kPlainItemWords + 2u) + state.together_object_slots;
}

/// What @p state leaves for the draws its batches' own culling does not list (CutDrawWords).
inline CutDrawWords instance_cut_draw_words(const InstanceCullPass& state)
{
    CutDrawWords words;
    words.quads_word = instance_quad_word(state);
    words.quad_capacity = state.quad_capacity;
    const uint32_t records = instance_item_records_word(state);
    if (records != kCullNoSlot) {
        for (const auto& own : state.own_items) {
            words.own_records.push_back({own.batch, records + own.word, own.count});
        }
    }
    words.own_clusters = state.own_clusters;
    return words;
}

/// The word of @p state's top tree for rays (kRayTopHeaderWords), or kCullNoSlot.
inline uint32_t instance_ray_top_word(const InstanceCullPass& state)
{
    return state.ray_top.valid() ? static_cast<uint32_t>(state.ray_top.offset() / sizeof(uint32_t)) : kCullNoSlot;
}

/// Tells the shaders of the view at @p view_globals_base where its top tree for rays is
/// (FrameGlobals::ray_top). The globals are written again every frame before the render path
/// runs, so this goes after every culling.
inline void publish_ray_top(const InstanceCullPass& state, uint32_t view_globals_base, FrameContext& ctx)
{
    if (!ctx.globals_arena) {
        return;
    }
    const uint32_t word = instance_ray_top_word(state);
    ctx.globals_arena->update_at(uint64_t(view_globals_base) * sizeof(FrameGlobals) + offsetof(FrameGlobals, ray_top), &word,
                                 sizeof(word));
}

/// The pixel VELK_TRACE_PIXEL=x,y names for the culling's trace (InstanceCullPass::trace), a
/// diagnostic logged every 60 frames; @p x is kTraceNone without it.
inline void trace_pixel_from_env(uint32_t& x, uint32_t& y)
{
    x = kTraceNone;
    y = 0;
    unsigned px = 0;
    unsigned py = 0;
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "VELK_TRACE_PIXEL") != 0 || !val) return;
    const bool read = std::sscanf(val, "%u,%u", &px, &py) == 2;
    std::free(val);
#else
    const char* val = std::getenv("VELK_TRACE_PIXEL");
    const bool read = val && std::sscanf(val, "%u,%u", &px, &py) == 2;
#endif
    if (read) {
        x = px;
        y = py;
    }
}

/// The cluster entry VELK_TRACE_ENTRY names, whose walks the trace notes wherever they go, as
/// the trace's log prints entries; kTraceNone without it.
inline uint32_t trace_entry_from_env()
{
    unsigned entry = 0;
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "VELK_TRACE_ENTRY") != 0 || !val) return kTraceNone;
    const bool read = std::sscanf(val, "%u", &entry) == 1;
    std::free(val);
#else
    const char* val = std::getenv("VELK_TRACE_ENTRY");
    const bool read = val && std::sscanf(val, "%u", &entry) == 1;
#endif
    return read ? entry : kTraceNone;
}

/// Tells the culling shaders of the view at @p view_globals_base where its trace is
/// (FrameGlobals::trace, kTraceNone for none). Before the culling runs, after the globals are
/// written for the frame.
inline void publish_trace(const InstanceCullPass& state, uint32_t view_globals_base, FrameContext& ctx)
{
    if (!ctx.globals_arena) {
        return;
    }
    const uint32_t word = state.trace.valid() && state.trace_x != kTraceNone
        ? static_cast<uint32_t>(state.trace.offset() / sizeof(uint32_t))
        : kTraceNone;
    ctx.globals_arena->update_at(uint64_t(view_globals_base) * sizeof(FrameGlobals) + offsetof(FrameGlobals, trace), &word,
                                 sizeof(word));
}

/// How many tree nodes the pool of @p state's kept cut of @p key holds, 0 without one.
inline uint32_t instance_cut_node_capacity(const InstanceCullPass& state, const void* key)
{
    for (const auto& cut : state.cuts) {
        if (cut.key == key) {
            return cut.node_capacity;
        }
    }
    return 0;
}

/// What an item of @p batch says for rays (kPlainItemWords 3 to 5): its draw's index in the draw
/// data, its ray shape kind (a triangle mesh is tested as one; anything else as its instance's
/// rect, unless its visual names an analytic shape of its own), and its mesh's ray data,
/// kInvalidMeshStaticBase until the mesh has it.
inline void batch_ray_words(IBatch& batch, FrameContext& ctx, uint32_t& draw, uint32_t& kind, uint32_t& mesh)
{
    draw = batch.draw_data_region_size() != 0 ? static_cast<uint32_t>(batch.draw_data_region_offset() / sizeof(DrawDataHeader))
                                              : kCullNoSlot;
    auto primitive_ptr = batch.primitive();
    const bool triangles = primitive_ptr && primitive_ptr->get_topology() == MeshTopology::TriangleList
                        && primitive_ptr->get_index_count() != 0;
    kind = triangles ? kRayShapeMesh : kRayShapeRect;
    if (auto* analytic = interface_cast<IAnalyticShape>(batch.shader_source().get())) {
        const uint32_t registered = ctx.snippets ? ctx.snippets->register_intersect(analytic, *ctx.render_ctx) : 0u;
        kind = registered != 0 ? registered : analytic->get_shape_kind();
    }
    mesh = kInvalidMeshStaticBase;
    if (auto* primitive = kind == kRayShapeMesh ? interface_cast<IMeshPrimitiveInternal>(primitive_ptr.get()) : nullptr) {
        mesh = primitive->ensure_rt_data(*ctx.resources);
    }
}

/**
 * @brief Adds @p batches' GPU culling to @p graph, if any of them are culled. Runs
 *        before the passes that draw them. With @p whole_cut, all of each kept cut is drawn, in
 *        view or not, for a view that shows another camera's choice. @p plain_table_word and
 *        @p plain_count are the scene's plain partitions (IBvh::get_plain_partitions): plain
 *        objects get a kept cut over them; without, plain objects are chosen every frame.
 */
/**
 * Everything a view's culling allocates this frame, and the work on the CPU that decides it:
 * called for every view before any view's passes are recorded (IRenderPath::reserve), since
 * growing the culling arena replaces the buffer that recorded passes name.
 * emit_instance_cull_pass then records what this worked out.
 */
inline void reserve_instance_cull(InstanceCullPass& state, const vector<IBatch::Ptr>* batches, FrameContext& ctx,
                                  uint32_t plain_table_word = kCullNoSlot, uint32_t plain_count = 0)
{
    VELK_PERF_SCOPE("instance_cull.reserve");
    state.reserved = false;
    if (!batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }

    bool record = false;
    // Last frame's recording named records again; this one does not.
    if (state.renamed) {
        state.renamed = false;
        state.renames.clear();
        record = true;
    }
    // An instanced visual with another number of slots than when its items were made: its
    // items made again.
    for (const auto& batch : *batches) {
        InstanceCullInfo info{};
        if (!batch || !batch->get_instance_cull(info) || !info.instanced) {
            continue;
        }
        const auto known = std::find_if(state.instanced_slots.begin(), state.instanced_slots.end(),
                                        [&](const auto& k) { return k.first == info.key; });
        if (known == state.instanced_slots.end() || known->second != info.slot_table_count) {
            if (!state.dirty) {
                VELK_LOG(I, "instance culling: items made again, a visual's slots %u, were %u", info.slot_table_count,
                         known == state.instanced_slots.end() ? 0u : known->second);
            }
            state.dirty = true;
        }
    }
    const bool batches_changed = state.dirty;
    // What the kept cuts' records named in the batches before they were made again.
    vector<InstanceCullPass::BatchTables> old_tables;
    vector<InstanceCullPass::ItemName> old_items;
    if (state.dirty) {
        VELK_PERF_SCOPE("instance_cull.items");
        state.dirty = false;
        record = true;
        state.count_words.clear();
        state.pending_meshes.clear();
        state.own_items.clear();
        state.own_clusters.clear();
        old_tables = std::move(state.batch_tables);
        old_items = std::move(state.item_names);
        state.batch_tables.clear();
        state.item_names.clear();
        // A batch's tables, read as it wrote them, once each.
        const auto note_tables = [&](const InstanceCullInfo& info) {
            for (const auto& t : state.batch_tables) {
                if (t.table_word == info.table_word) return;
            }
            InstanceCullPass::BatchTables t;
            t.key = info.cull_together ? nullptr : info.key;
            t.table_word = info.table_word;
            for (uint32_t m = 0; m < info.table_count; ++m) {
                uint32_t words[2] = {};
                arena->read_at((uint64_t(info.table_word) + uint64_t(m) * kBatchTableWords + kTableFirstEntry) * sizeof(uint32_t),
                               words, sizeof(words));
                t.first_entry.push_back(words[0]);
                t.entry_count.push_back(words[1]);
            }
            state.batch_tables.push_back(std::move(t));
        };
        vector<uint32_t> items;
        vector<uint32_t> item_objects;  ///< By item: its object id.
        vector<uint32_t> entries;
        uint64_t chosen = 0;
        uint64_t retest_records = 0;
        vector<const void*> counted;
        // A plain item for each instance of @p batch: its index, its slot, its table, its
        // draw, what rays test it as, its mesh's ray data and how far it can move
        // (kPlainItemWords), and its object id, the last word of its ElementInstance.
        const auto add_items = [&](IBatch& batch, uint32_t instances_base, uint32_t slot_count, uint32_t table_word) {
            const array_view<const uint8_t> instances = batch.instance_data();
            const uint32_t stride = batch.instance_stride();
            uint32_t draw;
            uint32_t kind;
            uint32_t mesh;
            batch_ray_words(batch, ctx, draw, kind, mesh);
            if (kind == kRayShapeMesh && mesh == kInvalidMeshStaticBase && slot_count != 0) {
                state.pending_meshes.push_back({batch.primitive(), static_cast<uint32_t>(items.size() / kPlainItemWords), slot_count});
            }
            for (uint32_t slot = 0; slot < slot_count; ++slot) {
                const size_t at = items.size();
                items.resize(at + kPlainItemWords, 0u);
                items[at + kPlainItemInstance] = instances_base + slot;
                items[at + kPlainItemSlot] = slot;
                items[at + kPlainItemTable] = table_word;
                items[at + kPlainItemDraw] = draw;
                items[at + kPlainItemKind] = kind;
                items[at + kPlainItemMesh] = mesh;
                uint32_t object = kCullNoSlot;
                if (stride >= 128u && size_t(slot + 1u) * stride <= instances.size()) {
                    std::memcpy(&object, instances.begin() + size_t(slot) * stride + 124u, sizeof(object));
                }
                item_objects.push_back(object);
                // Drawn by its own pass, its mesh is no part of what its record names.
                state.item_names.push_back({object, table_word != kCullNoSlot ? batch.primitive().get() : nullptr, 0u, table_word});
            }
        };
        // The instanced visuals, each once, whose items come after the plain ones: an item per
        // slot, its object id the visual's base plus the slot.
        struct InstancedVisual
        {
            const void* key;
            uint32_t slots;
            uint32_t object_base;
            uint32_t table_word;
            float displacement;
        };
        vector<InstancedVisual> instanced;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch) {
                continue;
            }
            if (!batch->get_instance_cull(info)) {
                // Drawn by its own pass, but rays see it, by its clusters when it has them for
                // that (a blended batch): an item with its table for the plain objects' kept cut,
                // whose clusters' draws the culling lays out for its pass (kLodRaysOnly).
                // Only a mesh: an analytic shape (a glyph) keeps its own intersect.
                uint32_t ray_draw;
                uint32_t ray_kind;
                uint32_t ray_mesh;
                batch_ray_words(*batch, ctx, ray_draw, ray_kind, ray_mesh);
                if (plain_table_word != kCullNoSlot && ray_kind == kRayShapeMesh && batch->get_ray_cull(info)) {
                    note_tables(info);
                    chosen += info.cluster_capacity;
                    entries.push_back(info.cluster_entry_word);
                    state.own_clusters.push_back(batch.get());
                    add_items(*batch, info.instances_base, info.slot_count, info.table_word);
                    continue;
                }
                // Else an item without a table.
                if (plain_table_word != kCullNoSlot && batch->instance_stride() == 128u && batch->instance_count() != 0
                    && batch->instance_region_size() != 0) {
                    state.own_items.push_back(
                        {batch.get(), static_cast<uint32_t>(items.size() / kPlainItemWords), batch->instance_count()});
                    add_items(*batch, static_cast<uint32_t>(batch->instance_region_offset() / 128u), batch->instance_count(),
                              kCullNoSlot);
                }
                continue;
            }
            if (std::find(counted.begin(), counted.end(), info.key) == counted.end()) {
                counted.push_back(info.key);
                retest_records += uint64_t(info.slot_count) + info.chosen_capacity + info.cluster_capacity;
            }
            note_tables(info);
            if (info.instanced) {
                // Its clusters go to the one chosen list and are laid out by their entry's draw;
                // a mesh without clusters is drawn from its visible list by its count.
                if (info.cluster_count != 0) {
                    chosen += info.cluster_capacity;
                    entries.push_back(info.cluster_entry_word);
                } else if (std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                           == state.count_words.end()) {
                    state.count_words.push_back(info.count_word);
                }
                if (std::find_if(instanced.begin(), instanced.end(), [&](const InstancedVisual& v) { return v.key == info.key; })
                    == instanced.end()) {
                    instanced.push_back({info.key, info.slot_table_count, info.object_base, info.table_word, info.max_displacement});
                }
            } else if (info.cull_together) {
                chosen += info.cluster_capacity;
                entries.push_back(info.cluster_entry_word);
                add_items(*batch, info.instances_base, info.slot_count, info.table_word);
            } else if (std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                       == state.count_words.end()) {
                state.count_words.push_back(info.count_word);
            }
        }
        // The instanced visuals' items, an item per slot after the plain items: instance_items_src
        // writes them whole each frame, with their objects and next words, from the visuals'
        // descriptors, so they are only counted here. The visuals keep their order, new ones
        // after, and each one's block of items is renamed as one.
        const uint32_t old_plain_items = state.instanced_first_item;
        const vector<std::pair<const void*, uint32_t>> old_instanced = std::move(state.instanced_slots);
        vector<std::pair<const void*, uint32_t>> instanced_slots;
        for (const auto& [key, slots] : old_instanced) {
            for (const InstancedVisual& v : instanced) {
                if (v.key == key) instanced_slots.push_back({v.key, v.slots});
            }
        }
        for (const InstancedVisual& v : instanced) {
            const bool known = std::find_if(instanced_slots.begin(), instanced_slots.end(),
                                            [&](const std::pair<const void*, uint32_t>& s) { return s.first == v.key; })
                            != instanced_slots.end();
            if (!known) instanced_slots.push_back({v.key, v.slots});
        }
        state.items_move = false;
        uint32_t instanced_items = 0;
        for (const InstancedVisual& v : instanced) {
            state.items_move = state.items_move || v.displacement > 0.f;
            instanced_items += v.slots;
        }
        state.instanced_slots = std::move(instanced_slots);
        state.instanced_first_item = static_cast<uint32_t>(items.size() / kPlainItemWords);
        state.instanced_items = instanced_items;
        // Each visual still here: its old first item, the slots that carry over, its new first.
        vector<uint32_t> blocks;
        {
            uint32_t old_first = old_plain_items;
            for (const auto& [key, old_slots] : old_instanced) {
                uint32_t new_first = state.instanced_first_item;
                for (const auto& [new_key, new_slots] : state.instanced_slots) {
                    if (new_key == key) {
                        blocks.push_back(old_first);
                        blocks.push_back(std::min(old_slots, new_slots));
                        blocks.push_back(new_first);
                        break;
                    }
                    new_first += new_slots;
                }
                old_first += old_slots;
            }
        }
        // Which of its object's items with that mesh each item is.
        {
            vector<uint32_t> order(state.item_names.size());
            for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
            const auto& names = state.item_names;
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                if (names[a].object != names[b].object) return names[a].object < names[b].object;
                if (names[a].primitive != names[b].primitive) return names[a].primitive < names[b].primitive;
                return a < b;
            });
            for (size_t k = 0; k < order.size(); ++k) {
                const bool same = k > 0 && names[order[k]].object == names[order[k - 1]].object
                               && names[order[k]].primitive == names[order[k - 1]].primitive;
                state.item_names[order[k]].ordinal = same ? state.item_names[order[k - 1]].ordinal + 1u : 0u;
            }
        }

        // What each kept cut's records name in the old batches, named in the new: each visual's
        // tables, and the plain items with theirs. A cut whose names do not all carry over is made
        // again below.
        state.renames.clear();
        const auto find_tables = [](const vector<InstanceCullPass::BatchTables>& all, uint32_t word) {
            const InstanceCullPass::BatchTables* found = nullptr;
            for (const auto& t : all) {
                found = t.table_word == word ? &t : found;
            }
            return found;
        };
        const auto pair_tables = [](const InstanceCullPass::BatchTables& from, const InstanceCullPass::BatchTables& to,
                                    vector<std::pair<uint32_t, uint32_t>>& pairs) {
            if (from.first_entry.size() != to.first_entry.size() || from.entry_count != to.entry_count) return false;
            for (uint32_t m = 0; m < from.first_entry.size(); ++m) {
                pairs.push_back({from.table_word + m * kBatchTableWords, to.table_word + m * kBatchTableWords});
                for (uint32_t e = 0; e < from.entry_count[m]; ++e) {
                    pairs.push_back({from.first_entry[m] + e * kClusterEntryWords, to.first_entry[m] + e * kClusterEntryWords});
                }
            }
            return true;
        };
        const auto add_rename = [&](const void* key, vector<std::pair<uint32_t, uint32_t>>& pairs, vector<uint32_t> item_map,
                                    vector<uint32_t> blocks) {
            std::sort(pairs.begin(), pairs.end());
            InstanceCullPass::Rename rename;
            rename.key = key;
            for (const auto& [from, to] : pairs) {
                rename.pairs.push_back(from);
                rename.pairs.push_back(to);
            }
            rename.items = std::move(item_map);
            rename.blocks = std::move(blocks);
            state.renames.push_back(std::move(rename));
        };
        for (const auto& from : old_tables) {
            if (!from.key) continue;
            for (const auto& to : state.batch_tables) {
                vector<std::pair<uint32_t, uint32_t>> pairs;
                if (to.key == from.key && pair_tables(from, to, pairs)) {
                    add_rename(from.key, pairs, {}, {});
                    break;
                }
            }
        }
        // The plain items by name; the instanced visuals' items, while those are as they were, as
        // a block after the plain items, moved by however many more or fewer plain items there
        // are. Instanced visuals that changed make the cut again.
        if (!old_items.empty() || !old_instanced.empty()) {
            // The new items by object, mesh and which of those, to look the old ones up in.
            vector<uint32_t> by_name(state.item_names.size());
            for (uint32_t i = 0; i < by_name.size(); ++i) by_name[i] = i;
            const auto& names = state.item_names;
            const auto less = [](const InstanceCullPass::ItemName& a, const InstanceCullPass::ItemName& b) {
                if (a.object != b.object) return a.object < b.object;
                if (a.primitive != b.primitive) return a.primitive < b.primitive;
                return a.ordinal < b.ordinal;
            };
            std::sort(by_name.begin(), by_name.end(), [&](uint32_t a, uint32_t b) { return less(names[a], names[b]); });
            // What carries over is named again; what does not (an item gone, a table whose entries
            // changed) is left out, and the partitions whose records name it choose again
            // (instance_cut_stale_src).
            vector<uint32_t> item_map(old_items.size(), kCullNoSlot);
            vector<std::pair<uint32_t, uint32_t>> table_pairs;
            vector<uint32_t> unpaired;  ///< Old tables that went to more than one new table.
            for (uint32_t i = 0; i < old_items.size(); ++i) {
                const auto& name = old_items[i];
                const auto* at = std::lower_bound(by_name.begin(), by_name.end(), name,
                                                  [&](uint32_t j, const InstanceCullPass::ItemName& n) { return less(names[j], n); });
                if (name.object == kCullNoSlot || at == by_name.end() || less(name, names[*at])) continue;
                item_map[i] = *at;
                if (name.table_word == kCullNoSlot) continue;
                const uint32_t to = names[*at].table_word;
                const auto* known = std::find_if(table_pairs.begin(), table_pairs.end(),
                                                 [&](const std::pair<uint32_t, uint32_t>& p) { return p.first == name.table_word; });
                if (known == table_pairs.end()) {
                    table_pairs.push_back({name.table_word, to});
                } else if (known->second != to) {
                    unpaired.push_back(name.table_word);
                }
            }
            vector<std::pair<uint32_t, uint32_t>> pairs;
            for (const auto& [from_word, to_word] : table_pairs) {
                if (std::find(unpaired.begin(), unpaired.end(), from_word) != unpaired.end()) continue;
                const auto* from = find_tables(old_tables, from_word);
                const auto* to = find_tables(state.batch_tables, to_word);
                if (from && to) pair_tables(*from, *to, pairs);
            }
            // The instanced visuals' tables, which the block's items name.
            for (const auto& [key, slots] : state.instanced_slots) {
                for (const auto& from : old_tables) {
                    if (from.key != key) continue;
                    const auto* to = std::find_if(state.batch_tables.begin(), state.batch_tables.end(),
                                                  [&](const InstanceCullPass::BatchTables& t) { return t.key == key; });
                    if (to != state.batch_tables.end()) pair_tables(from, *to, pairs);
                }
            }
            add_rename(&kPlainCutKey, pairs, std::move(item_map), std::move(blocks));
        }

        const uint32_t plain_items = static_cast<uint32_t>(items.size() / kPlainItemWords);
        state.together_items = plain_items + state.instanced_items;
        state.together_entries = static_cast<uint32_t>(entries.size());
        state.together_chosen = static_cast<uint32_t>(chosen);
        state.together = {};
        // Which plain items each object has, by the slot of its id: the first, then each one's
        // next. An instanced object is found through its visual's descriptor instead.
        uint32_t object_slots = 0;
        for (uint32_t object : item_objects) {
            if (object != kCullNoSlot) {
                object_slots = std::max(object_slots, (object & kObjectSlotMask) + 1u);
            }
        }
        vector<uint32_t> heads(object_slots, kCullNoSlot);
        vector<uint32_t> nexts(item_objects.size(), kCullNoSlot);
        for (uint32_t item = static_cast<uint32_t>(item_objects.size()); item-- > 0;) {
            const uint32_t object = item_objects[item];
            if (object != kCullNoSlot) {
                nexts[item] = heads[object & kObjectSlotMask];
                heads[object & kObjectSlotMask] = item;
            }
        }
        state.together_object_slots = object_slots;
        if (state.together_items != 0) {
            const uint64_t list_words = kCullChosenHeaderWords + chosen * kCullChosenRecordWords;
            const uint64_t all = state.together_items;
            // Entries, items, their objects and next words, the heads, then the item records,
            // written every frame. The instanced visuals' parts are written by instance_items_src.
            const uint64_t words = list_words + entries.size() + all * (kPlainItemWords + 3u) + heads.size();
            state.together = arena->alloc(words * sizeof(uint32_t));
            if (state.together.valid()) {
                const uint64_t entries_at = state.together.offset() + list_words * sizeof(uint32_t);
                const uint64_t items_at = entries_at + entries.size() * sizeof(uint32_t);
                const uint64_t objects_at = items_at + all * kPlainItemWords * sizeof(uint32_t);
                const uint64_t nexts_at = objects_at + all * sizeof(uint32_t);
                const uint64_t heads_at = nexts_at + all * sizeof(uint32_t);
                arena->write_at(entries_at, entries.data(), entries.size() * sizeof(uint32_t));
                arena->write_at(items_at, items.data(), items.size() * sizeof(uint32_t));
                arena->write_at(objects_at, item_objects.data(), item_objects.size() * sizeof(uint32_t));
                arena->write_at(nexts_at, nexts.data(), nexts.size() * sizeof(uint32_t));
                arena->write_at(heads_at, heads.data(), heads.size() * sizeof(uint32_t));
            } else {
                state.together_items = 0;
                state.together_entries = 0;
            }
        }
        state.retest_capacity = static_cast<uint32_t>(std::min<uint64_t>(retest_records, kCullRetestMaxRecords));
        state.retest = arena->alloc((uint64_t(kCullRetestHeaderWords) + uint64_t(state.retest_capacity) * kCullRetestRecordWords)
                                    * sizeof(uint32_t));
        if (!state.retest.valid()) {
            state.retest_capacity = 0;
        }
    }
    // Meshes whose ray data has come since their items were made.
    if (!state.pending_meshes.empty() && state.together.valid()) {
        const uint64_t items_byte = state.together.offset()
            + (uint64_t(kCullChosenHeaderWords) + uint64_t(state.together_chosen) * kCullChosenRecordWords
               + state.together_entries) * sizeof(uint32_t);
        for (size_t i = 0; i < state.pending_meshes.size();) {
            const auto& pending = state.pending_meshes[i];
            auto* primitive = interface_cast<IMeshPrimitiveInternal>(pending.primitive.get());
            const uint32_t mesh = primitive ? primitive->ensure_rt_data(*ctx.resources) : kInvalidMeshStaticBase;
            if (mesh == kInvalidMeshStaticBase && primitive) {
                ++i;
                continue;
            }
            for (uint32_t k = 0; k < pending.count; ++k) {
                arena->update_at(items_byte + (uint64_t(pending.first_item + k) * kPlainItemWords + kPlainItemMesh) * sizeof(uint32_t),
                                 &mesh, sizeof(mesh));
            }
            state.pending_meshes.erase(state.pending_meshes.begin() + static_cast<ptrdiff_t>(i));
        }
    }
    // The instanced visuals' descriptors, which instance_items_src reads to write their items:
    // made each frame from their batches, since a slot table moves as its visual grows and a
    // mesh's ray data can come late, and written when they change.
    {
        VELK_PERF_SCOPE("instance_cull.descriptors");
        vector<uint32_t> descriptors;
        uint32_t first = state.instanced_first_item;
        for (const auto& [key, slots] : state.instanced_slots) {
            const size_t header = descriptors.size();
            descriptors.resize(header + kItemDescriptorHeaderWords, 0u);
            descriptors[header + kItemDescriptorFirst] = first;
            descriptors[header + kItemDescriptorSlots] = slots;
            uint32_t meshes = 0;
            for (const auto& batch : *batches) {
                InstanceCullInfo info{};
                if (!batch || !batch->get_instance_cull(info) || !info.instanced || info.key != key) {
                    continue;
                }
                descriptors[header + kItemDescriptorInstances] = info.instances_base;
                descriptors[header + kItemDescriptorSlotTable] = info.slot_table_count >= slots ? info.slot_table_word : 0u;
                descriptors[header + kItemDescriptorTable] = info.table_word;
                std::memcpy(&descriptors[header + kItemDescriptorMotion], &info.max_displacement, sizeof(uint32_t));
                descriptors[header + kItemDescriptorObjectBase] = info.object_base != 0u ? info.object_base : kCullNoSlot;
                if (info.mesh_index >= meshes) {
                    descriptors.resize(header + kItemDescriptorHeaderWords + size_t(info.mesh_index + 1u) * kItemMeshWords, 0u);
                    for (uint32_t m = meshes; m <= info.mesh_index; ++m) {
                        uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(m) * kItemMeshWords;
                        words[kItemMeshDraw] = kCullNoSlot;
                        words[kItemMeshKind] = kRayShapeMesh;
                        words[kItemMeshMesh] = kInvalidMeshStaticBase;
                    }
                    meshes = info.mesh_index + 1u;
                }
                uint32_t* words = descriptors.data() + header + kItemDescriptorHeaderWords + size_t(info.mesh_index) * kItemMeshWords;
                batch_ray_words(*batch, ctx, words[kItemMeshDraw], words[kItemMeshKind], words[kItemMeshMesh]);
            }
            descriptors[header + kItemDescriptorMeshes] = meshes;
            first += slots;
        }
        if (descriptors != state.item_descriptors) {
            const uint64_t bytes = descriptors.size() * sizeof(uint32_t);
            const bool fresh = !state.item_descriptor_region.valid() || state.item_descriptor_region.size() < bytes;
            if (fresh && bytes != 0) {
                state.item_descriptor_region = arena->alloc(bytes * 2u);
                record = true;
            }
            if (state.item_descriptor_region.valid() && bytes != 0) {
                if (fresh) {
                    arena->write_at(state.item_descriptor_region.offset(), descriptors.data(), bytes);
                } else {
                    arena->update_at(state.item_descriptor_region.offset(), descriptors.data(), bytes);
                }
            }
            record = record || state.item_descriptor_count != state.instanced_slots.size();
            state.item_descriptor_count = static_cast<uint32_t>(state.instanced_slots.size());
            state.item_descriptors = std::move(descriptors);
        }
    }

    if (state.count_words.empty() && state.together_items == 0) {
        return;
    }

    // The BVH replaces the scene's table when it fills up.
    uint64_t partition_tables = 0;
    partition_tables = partition_tables * 31u + plain_table_word;
    partition_tables = partition_tables * 31u + plain_count;
    // The scene's objects, plain and instanced, get a kept cut when the scene has partitions.
    const bool plain_cut = plain_table_word != kCullNoSlot && plain_count != 0 && state.together_items != 0;
    const bool tables_changed = partition_tables != state.partition_tables;
    record = record || tables_changed;
    state.partition_tables = partition_tables;

    // Whether a cut's pool was too small lately: every frame for kCutYoungFrames after it is made
    // and while partitions ask for pages they do not get, else now and then, reading the arena
    // being slow. Until it grows, partitions that find no free pages keep their old records.
    constexpr uint64_t kCutCheckFrames = 16;
    constexpr uint64_t kCutYoungFrames = 32;
    if (!state.cuts.empty()) {
        const bool now_and_then = ctx.present_counter >= state.cut_check_frame + kCutCheckFrames;
        if (now_and_then) {
            state.cut_check_frame = ctx.present_counter;
        }
        for (auto& cut : state.cuts) {
            if (!now_and_then && !cut.short_pages && ctx.present_counter >= cut.made_frame + kCutYoungFrames) {
                continue;
            }
            uint32_t short_pages = 0;
            arena->read_at(cut.region.offset() + uint64_t(kCutShortPages) * sizeof(uint32_t), &short_pages, sizeof(short_pages));
            cut.short_pages = short_pages != 0;
            uint32_t wanted_pages = 0;
            arena->read_at(cut.region.offset() + uint64_t(kCutPagesWanted) * sizeof(uint32_t), &wanted_pages, sizeof(wanted_pages));
            const uint32_t wanted = wanted_pages * kCutPageRecords;
            if (wanted > cut.capacity) {
                state.cut_growth.push_back({cut.key, wanted});
            }
        }
    }

    // The scene's partitions get a kept cut for this view, every partition choosing on its first
    // frame. It is made again when the partitions may have been laid out again, so that none
    // keeps records meant for another, or when its pool needs room; else it carries over.
    // (`dirty` also follows the camera, so it alone says nothing about partitions.)
    if (batches_changed || tables_changed || !state.cut_growth.empty()) {
        record = true;
        // What gets a cut: the scene's objects, by their items, whose indices change with the
        // batches.
        struct Wants
        {
            const void* key;
            uint32_t partitions;
            uint32_t members;  ///< Objects, which its partitions' chunks hold.
            bool by_item;
        };
        vector<Wants> wants;
        if (plain_cut) {
            wants.push_back({&kPlainCutKey, plain_count, state.together_items, true});
        }
        vector<InstanceCullPass::Cut> kept;
        vector<const void*> renamed_keys;  ///< Cuts kept across batches made again.
        for (const Wants& info : wants) {
            uint32_t wanted = 0;
            for (const auto& [key, records] : state.cut_growth) {
                wanted = key == info.key ? std::max(wanted, records) : wanted;
            }
            auto old = std::find_if(state.cuts.begin(), state.cuts.end(),
                                    [&](const InstanceCullPass::Cut& c) { return c.key == info.key; });
            // Batches made again keep a cut whose records can all be named in the new ones.
            const bool named = !batches_changed
                            || std::find_if(state.renames.begin(), state.renames.end(),
                                            [&](const InstanceCullPass::Rename& r) { return r.key == info.key; })
                                   != state.renames.end();
            if (!tables_changed && wanted == 0 && named && old != state.cuts.end() && old->partitions == info.partitions) {
                kept.push_back(std::move(*old));
                if (batches_changed) {
                    renamed_keys.push_back(info.key);
                }
                continue;
            }
            // Made again from nothing: every record chooses again and its light starts over.
            if (old != state.cuts.end()) {
                VELK_LOG(I, "kept cut made again:%s%s%s%s", tables_changed ? " tables changed" : "",
                         wanted != 0 ? " out of room" : "", !named ? " its batches' words did not carry over" : "",
                         old->partitions != info.partitions ? " partitions changed" : "");
            }
            InstanceCullPass::Cut cut;
            cut.key = info.key;
            cut.partitions = info.partitions;
            cut.made_frame = ctx.present_counter;
            // The pool: whole pages, room for the records kept, and for the new and the old records
            // of the partitions choosing again at once, with a page a partition for the part-filled
            // last pages of both.
            const uint32_t capacity = std::max({info.members * 2u + 4096u + info.partitions * kCutPageRecords, wanted + wanted / 2u,
                                                old != state.cuts.end() ? old->capacity : 0u});
            const uint32_t pages = (capacity + kCutPageRecords - 1u) / kCutPageRecords;
            cut.capacity = pages * kCutPageRecords;
            cut.node_capacity = pages * kCutPageNodes;
            // Every partition's chunks at most: each one's members, and a part-filled chunk each.
            const uint32_t chunks = (info.members + kCullChunkSlots - 1u) / kCullChunkSlots + info.partitions;
            const uint32_t choose_word = kCutHeaderWords;
            const uint32_t counts_word = choose_word + chunks * kCullChunkListWords;
            const uint32_t state_word = counts_word + chunks;
            // The pages' headers, the free stack and the page marks, then the two page tables.
            const uint32_t headers_word = state_word + info.partitions * kCutPartitionWords;
            const uint32_t stack_word = headers_word + pages * kCutPageWords;
            const uint32_t page_marks_word = stack_word + pages;
            const uint32_t tables_word = page_marks_word + pages / 32u + 1u;
            // The pool's records, ray list entries and tree nodes.
            const uint32_t record_pool = tables_word + pages * 2u;
            const uint32_t ray_pool = record_pool + cut.capacity * kCutRecordWords;
            const uint32_t node_pool = ray_pool + cut.capacity * kCutRayWords;
            // The takeover table: two entries a record (by what it is, and by the group it was made
            // from), at least two slots each, so that lookups stay short.
            uint32_t takeover_slots = 1;
            while (takeover_slots < cut.capacity * 4u) takeover_slots <<= 1;
            const uint32_t takeover_word = node_pool + cut.node_capacity * kCutNodeWords;
            // The ray marks, a bit a record of the pool.
            const uint32_t marks_word = takeover_word + takeover_slots * kCutTakeoverWords;
            // The runs the drawing walks, two words each.
            const uint32_t visible_word = marks_word + cut.capacity / 32u + 1u;
            // The sort of its ray lists past kRaySortMax.
            const uint32_t large_word = visible_word + cut_visible_runs(cut.capacity, info.partitions) * 2u;
            const size_t words = size_t(large_word) + size_t(cut_large_words(info.partitions, kCutLargeRoom));
            cut.region = arena->alloc(words * sizeof(uint32_t));
            if (!cut.region.valid()) {
                VELK_LOG(E, "instance culling: no room for a kept cut of %u records", cut.capacity);
                continue;
            }
            const auto base = static_cast<uint32_t>(cut.region.offset() / sizeof(uint32_t));
            vector<uint32_t> initial;
            initial.resize(tables_word, 0u);
            for (uint32_t args : {kCutChooseGroups, kCutDrawGroups, kCutVisibleGroups}) {
                initial[args + 1u] = 1u;
                initial[args + 2u] = 1u;
            }
            initial[kCutPages] = pages;
            initial[kCutFreePages] = pages;
            initial[kCutCapacity] = cut.capacity;
            initial[kCutRecordPool] = base + record_pool;
            initial[kCutRayPool] = base + ray_pool;
            initial[kCutNodePool] = base + node_pool;
            initial[kCutPageHeaders] = base + headers_word;
            initial[kCutStackWord] = base + stack_word;
            initial[kCutPageMarks] = base + page_marks_word;
            initial[kCutTables] = base + tables_word;
            initial[kCutTables + 1u] = base + tables_word + pages;
            initial[kCutPartitions] = info.partitions;
            initial[kCutChooseWord] = base + choose_word;
            initial[kCutStatesWord] = base + state_word;
            initial[kCutChunkCountsWord] = base + counts_word;
            initial[kCutTakeoverWord] = base + takeover_word;
            initial[kCutTakeoverSlots] = takeover_slots;
            initial[kCutRayMarksWord] = base + marks_word;
            initial[kCutVisibleWord] = base + visible_word;
            initial[kCutLargeWord] = base + large_word;
            cut.large_word = base + large_word;
            // Every page free, the lowest taken first; the marks clear.
            for (uint32_t p = 0; p < pages; ++p) {
                initial[headers_word + p * kCutPageWords + kCutPageOwner] = kCullNoSlot;
                initial[stack_word + p] = pages - 1u - p;
            }
            // Chosen from nowhere, and from a table entry no partition has: all choose.
            constexpr float kFar = 1e30f;
            uint32_t far_bits;
            std::memcpy(&far_bits, &kFar, sizeof(far_bits));
            for (uint32_t p = 0; p < info.partitions; ++p) {
                uint32_t* partition = initial.data() + state_word + p * kCutPartitionWords;
                partition[kPartFrom] = partition[kPartFrom + 1u] = partition[kPartFrom + 2u] = far_bits;
                partition[kPartChunkFirst] = kCullNoSlot;
            }
            arena->write_at(cut.region.offset(), initial.data(), initial.size() * sizeof(uint32_t));
            // The long lists' sort: nothing queued, its room, and its dispatches' other group counts.
            {
                uint32_t large[kCutLargeHeaderWords] = {};
                large[kCutLargeCap] = kCutLargeRoom;
                for (uint32_t args : {kCutLargeListGroups, kCutLargeBlockGroups}) {
                    large[args + 1u] = 1u;
                    large[args + 2u] = 1u;
                }
                arena->write_at(cut.region.offset() + uint64_t(large_word) * sizeof(uint32_t), large, sizeof(large));
            }
            // The takeover table starts empty: no slot of frame 0, which the begin pass leaves at once.
            vector<uint32_t> empty_table(size_t(takeover_slots) * kCutTakeoverWords, 0u);
            arena->write_at(cut.region.offset() + uint64_t(takeover_word) * sizeof(uint32_t), empty_table.data(),
                            empty_table.size() * sizeof(uint32_t));
            kept.push_back(std::move(cut));
            // The total of the counts is not comparable with the last one any more.
            state.cut_log_frame = ctx.present_counter;
            state.cut_moves_known = false;
        }
        state.cuts = std::move(kept);
        state.cut_growth.clear();

        // The kept cuts' records named in the new batches, by the recording, this frame only.
        vector<InstanceCullPass::Rename> renames;
        uint64_t rename_words = 0;
        for (auto& rename : state.renames) {
            if (std::find(renamed_keys.begin(), renamed_keys.end(), rename.key) != renamed_keys.end()) {
                rename_words += rename.pairs.size() + rename.items.size() + rename.blocks.size();
                renames.push_back(std::move(rename));
            }
        }
        state.renames = std::move(renames);
        state.rename_words = rename_words != 0 ? arena->alloc(rename_words * sizeof(uint32_t)) : ArenaRegion{};
        if (!state.rename_words.valid()) {
            state.renames.clear();
        }
        uint64_t at = state.rename_words.valid() ? state.rename_words.offset() / sizeof(uint32_t) : 0u;
        for (auto& rename : state.renames) {
            rename.pairs_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.pairs.data(), rename.pairs.size() * sizeof(uint32_t));
            at += rename.pairs.size();
            rename.items_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.items.data(), rename.items.size() * sizeof(uint32_t));
            at += rename.items.size();
            rename.blocks_word = static_cast<uint32_t>(at);
            arena->write_at(at * sizeof(uint32_t), rename.blocks.data(), rename.blocks.size() * sizeof(uint32_t));
            at += rename.blocks.size();
        }
        state.renamed = !state.renames.empty();

        // The view's quad list: room for a quad per member of every kept cut, instances and
        // plain objects alike, which no set of quads that each stand for members of their own
        // can pass, and one per partition, whose chunks may be part filled.
        uint32_t quads = 0;
        for (const Wants& info : wants) {
            quads += info.members + info.partitions;
        }
        if (quads != state.quad_capacity) {
            state.quad_capacity = 0;
            // A quad's word, and after them all each one's record.
            state.quads = quads != 0 ? arena->alloc((uint64_t(kQuadListHeaderWords) + uint64_t(quads) * 2u) * sizeof(uint32_t))
                                     : ArenaRegion{};
            if (state.quads.valid()) {
                uint32_t header[kQuadListHeaderWords] = {};
                header[kQuadListVertices] = 4u;
                header[kQuadListDrawCount] = 1u;
                header[kQuadListDraw] = kCullNoSlot;
                arena->write_at(state.quads.offset(), header, sizeof(header));
                state.quad_capacity = quads;
            }
        }
    }
    const auto cut_word = [&](const void* key) {
        for (const auto& c : state.cuts) {
            if (c.key == key) {
                return static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
            }
        }
        return kCullNoSlot;
    };

    // How often partitions chose again lately, now and then.
    constexpr uint64_t kCutLogFrames = 120;
    if (!state.cuts.empty() && ctx.present_counter >= state.cut_log_frame + kCutLogFrames) {
        uint32_t moves = 0;
        uint32_t partitions = 0;
        uint32_t records = 0;
        uint32_t for_rays = 0;
        uint32_t pages = 0;
        uint32_t pages_used = 0;
        for (const auto& c : state.cuts) {
            uint32_t header[kCutHeaderWords] = {};
            arena->read_at(c.region.offset(), header, sizeof(header));
            moves += header[kCutChoseAgain];
            partitions += c.partitions;
            const uint32_t light = header[kCutLastForRaysRecords];
            for_rays += light;
            pages += header[kCutPages];
            pages_used += header[kCutPages] - std::min(header[kCutFreePages], header[kCutPages]);
            vector<uint32_t> states(size_t(c.partitions) * kCutPartitionWords);
            arena->read_at(uint64_t(header[kCutStatesWord]) * sizeof(uint32_t), states.data(), states.size() * sizeof(uint32_t));
            for (uint32_t p = 0; p < c.partitions; ++p) records += states[size_t(p) * kCutPartitionWords + kPartCount];
        }
        const auto frames = static_cast<double>(ctx.present_counter - state.cut_log_frame);
        if (state.cut_moves_known) {
            VELK_LOG(I, "kept cut: %u records on %u of %u pages, %u for rays only; partitions choosing again: %.1f a frame, "
                        "of %u table entries",
                     records, pages_used, pages, for_rays, static_cast<double>(moves - state.cut_moves) / frames, partitions);
            if (state.quads.valid()) {
                uint32_t quads = 0;
                uint32_t in_cut = 0;
                arena->read_at(state.quads.offset() + kQuadListCount * sizeof(uint32_t), &quads, sizeof(quads));
                arena->read_at(state.quads.offset() + kQuadListInCuts * sizeof(uint32_t), &in_cut, sizeof(in_cut));
                VELK_LOG(I, "quads %u in cut, %u in view", in_cut, quads);
            }
        }
        state.cut_moves = moves;
        state.cut_moves_known = true;
        state.cut_log_frame = ctx.present_counter;
    }

    // The trace of one pixel, while the path asks for one: made at once; its notes logged every
    // 60 frames, summed by what they say, then cleared.
    if (state.trace_x != kTraceNone) {
        if (!state.trace.valid()) {
            state.trace = arena->alloc((size_t(kTraceHeaderWords) + size_t(kTraceCapacity) * kTraceNoteWords) * sizeof(uint32_t));
            if (state.trace.valid()) {
                const uint32_t header[kTraceHeaderWords] = {state.trace_x, state.trace_y, 0u, kTraceCapacity, state.trace_entry, 0u};
                arena->update_at(state.trace.offset(), header, sizeof(header));
                state.trace_log_frame = ctx.present_counter;
            }
        } else if (ctx.present_counter >= state.trace_log_frame + 60u) {
            state.trace_log_frame = ctx.present_counter;
            uint32_t count = 0;
            arena->read_at(state.trace.offset() + 2u * sizeof(uint32_t), &count, sizeof(count));
            const uint32_t made = count;
            count = std::min(count, kTraceCapacity);
            vector<uint32_t> notes(size_t(count) * kTraceNoteWords);
            if (count != 0) {
                arena->read_at(state.trace.offset() + kTraceHeaderWords * sizeof(uint32_t), notes.data(),
                               notes.size() * sizeof(uint32_t));
            }
            const uint32_t none = 0;
            arena->update_at(state.trace.offset() + 2u * sizeof(uint32_t), &none, sizeof(none));
            // Summed by what came of what: stage and outcome, then entry and cluster, or partition.
            struct Sum
            {
                uint32_t what, a, b, c, n;
            };
            vector<Sum> sums;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t* n = notes.data() + size_t(i) * kTraceNoteWords;
                auto it = std::find_if(sums.begin(), sums.end(),
                                       [&](const Sum& s) { return s.what == n[0] && s.a == n[1] && s.b == n[2] && s.c == n[3]; });
                if (it == sums.end()) {
                    sums.push_back({n[0], n[1], n[2], n[3], 1u});
                } else {
                    ++it->n;
                }
            }
            std::sort(sums.begin(), sums.end(), [](const Sum& l, const Sum& r) {
                return l.what != r.what ? (l.what & 255u) < (r.what & 255u) || ((l.what & 255u) == (r.what & 255u) && l.what < r.what)
                                        : l.a != r.a ? l.a < r.a : l.b < r.b;
            });
            VELK_LOG(I, "trace at pixel %u,%u: %u notes since the last log%s", state.trace_x, state.trace_y, made,
                     made > kTraceCapacity ? " (only the first kept)" : "");
            static constexpr const char* kChoose[] = {"chosen", "LEFT OUT past its count"};
            static constexpr const char* kPartition[] = {"listed", "OUTSIDE the view", "runs PAST THE ROOM"};
            static constexpr const char* kDraw[] = {"chosen", "OUTSIDE the view", "held back by occlusion", "compute raster",
                                                    "CHOSEN LIST FULL"};
            static constexpr const char* kRaster[] = {"reached the shader", "CUT OUT by its alpha", "written"};
            for (const Sum& s : sums) {
                const uint32_t stage = s.what & 255u;
                const uint32_t outcome = s.what >> 8u;
                if (stage == kTracePartition) {
                    VELK_LOG(I, "  partition %u (%u records): %s, %u times", s.a, s.b, outcome < 3u ? kPartition[outcome] : "?",
                             s.n);
                    continue;
                }
                if (stage == kTraceNumbers) {
                    float stored = 0.f;
                    float pixels = 0.f;
                    std::memcpy(&stored, &s.b, sizeof(stored));
                    std::memcpy(&pixels, &s.c, sizeof(pixels));
                    const uint32_t which = s.a & 0x0F000000u;
                    const uint32_t ref = s.a & ~0x0F000000u;
                    const char* name = which == 0x08000000u ? "least error under"
                                     : which == 0x04000000u ? "largest parent error under"
                                     : which == 0x02000000u ? "own error of cluster"
                                                            : "parent error of cluster";
                    VELK_LOG(I, "  numbers: %s %s %u: stored %g, %.3f px, %u times", name,
                             which >= 0x04000000u ? ((ref & 0x80000000u) != 0u ? "leaf" : "node") : "",
                             ref & 0x7FFFFFFFu, stored, pixels, s.n);
                    continue;
                }
                if (stage == kTraceShape) {
                    VELK_LOG(I, "  shape: %s %u is child %u of node %u", (s.a & 0x80000000u) != 0u ? "cluster" : "node",
                             s.a & 0x7FFFFFFFu, s.c, s.b);
                    continue;
                }
                if (stage == kTraceInstance) {
                    static constexpr const char* kInstance[] = {"walked", "TOO FAR", "OUTSIDE the view", "held back by occlusion"};
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        InstanceCullInfo info{};
                        if (batch && batch->get_instance_cull(info) && info.cluster_entry_word == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    VELK_LOG(I, "  instance of entry %u (%u triangles, %u entries) slot %u, culled every frame: %s, %u times", s.a,
                             triangles, s.c, s.b, outcome < 4u ? kInstance[outcome] : "?", s.n);
                    continue;
                }
                if (stage == kTraceBlended) {
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    float opacity = 0.f;
                    std::memcpy(&opacity, &s.c, sizeof(opacity));
                    VELK_LOG(I, "  blended draw %u (%u triangles) ids %08x drawn over it with opacity %.3f, %u times", s.a,
                             triangles, s.b, opacity, s.n);
                    continue;
                }
                if (stage == kTraceResolve) {
                    VELK_LOG(I, "  resolve: took the %s raster's ids %08x %08x (draw %d), %u times",
                             outcome == 0u ? "hardware" : "compute", s.a, s.b,
                             s.a == 0u ? -1 : int((s.a & 0x007FFFFFu) - 1u), s.n);
                    continue;
                }
                if (stage == kTraceForward) {
                    uint32_t triangles = 0;
                    bool blended = false;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            blended = batch->pipeline_options().blend_mode == BlendMode::Alpha;
                            break;
                        }
                    }
                    float depth = 0.f;
                    std::memcpy(&depth, &s.c, sizeof(depth));
                    VELK_LOG(I, "  forward draw %u (%u triangles%s) ids %08x depth %.6f: drawn, %u times", s.a, triangles,
                             blended ? ", blended" : "", s.b, depth, s.n);
                    continue;
                }
                if (stage == kTraceRaster) {
                    // The batch whose draw record it is: its triangles, and whether blended.
                    uint32_t triangles = 0;
                    for (const auto& batch : *batches) {
                        if (batch && batch->draw_data_region_offset() / sizeof(DrawDataHeader) == s.a) {
                            if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                            break;
                        }
                    }
                    float depth = 0.f;
                    std::memcpy(&depth, &s.c, sizeof(depth));
                    VELK_LOG(I, "  raster draw %u (%u triangles) ids %08x depth %.6f: %s, %u times", s.a, triangles, s.b, depth,
                             outcome < 3u ? kRaster[outcome] : "?", s.n);
                    continue;
                }
                // The batch whose entry it is: its triangles, and whether blended.
                uint32_t triangles = 0;
                bool blended = false;
                uint64_t draw = 0;
                for (const auto& batch : *batches) {
                    InstanceCullInfo info{};
                    if (!batch || !(batch->get_instance_cull(info) || batch->get_ray_cull(info))
                        || info.cluster_entry_word != s.a) {
                        continue;
                    }
                    if (auto* primitive = batch->primitive().get()) triangles = primitive->get_index_count() / 3u;
                    blended = batch->pipeline_options().blend_mode == BlendMode::Alpha;
                    draw = batch->draw_data_region_offset() / sizeof(DrawDataHeader);
                    break;
                }
                static constexpr const char* kWalk[] = {"?", "?", "chosen", "for rays only", "skipped, all under its node too coarse",
                                                        "skipped, all under its node have a parent that will do",
                                                        "too coarse", "its parent will do",
                                                        "NODE skipped, all under it too coarse",
                                                        "NODE skipped, all under it have a parent that will do",
                                                        "NODE skipped, out of view, all under it have a parent at the light's level",
                                                        "chosen as a root record"};
                const char* how = stage == kTraceChoose ? (outcome < 2u ? kChoose[outcome] : "?")
                                : stage == kTraceDraw  ? (outcome < 5u ? kDraw[outcome] : "?")
                                : stage == kTraceWalk  ? (outcome < 12u ? kWalk[outcome] : "?")
                                                       : "?";
                if (stage == kTraceWalk) {
                    VELK_LOG(I, "  walk entry %u (draw %llu, %u triangles) %s %u, partition state %u: %s, %u times", s.a,
                             static_cast<unsigned long long>(draw), triangles, (s.b & 0x40000000u) != 0u ? "node" : "cluster",
                             s.b & ~0x40000000u, s.c, how, s.n);
                    continue;
                }
                VELK_LOG(I, "  %s entry %u (draw %llu, %u triangles%s) cluster %u: %s, %u times",
                         stage == kTraceChoose ? "choose" : stage == kTraceDraw ? "draw" : "?", s.a,
                         static_cast<unsigned long long>(draw), triangles, blended ? ", blended" : "", s.b, how, s.n);
            }
        }
    }

    // The light pool, with a segment to start, and more when records asked for pages that were
    // not there. Its pages are swept every frame, so a new segment's are free on the next.
    if (!state.cuts.empty() && ensure_light_pool(state.light_pool, *arena, ctx.backend->device_memory_bytes(), ctx.backend->compute_fill_threads())) {
        LightPool& pool = state.light_pool;
        // Its budget in rays, by what the view's trace took: its passes' GPU time, the one values'
        // and the points'.
        const char* trace_passes[2] = {state.chosen_by_other ? "other:deferred.light_trace" : "deferred.light_trace",
                                       state.chosen_by_other ? "other:deferred.light_points" : "deferred.light_points"};
        float trace_ms = 0.f;
        for (const auto& t : ctx.backend->last_gpu_timings()) {
            if (std::strcmp(t.label, trace_passes[0]) == 0 || std::strcmp(t.label, trace_passes[1]) == 0) {
                trace_ms += t.ms;
            }
        }
        adjust_light_rays(pool, *arena, trace_ms, state.light_trace_ms);
        if (pool.segments.empty()) {
            // A development aid: VELK_LIGHT_POOL_START starts the pool with that many segments.
            uint32_t start = 1u;
            if (const char* text = std::getenv("VELK_LIGHT_POOL_START")) {
                const long n = std::strtol(text, nullptr, 10);
                if (n > 1) start = static_cast<uint32_t>(n);
            }
            grow_light_pool(pool, *arena, start);
            VELK_LOG(I, "light pool: the trace fills the device at %u threads", pool.fill);
        } else if (ctx.present_counter >= pool.check_frame + 4u) {
            pool.check_frame = ctx.present_counter;
            // The most any frame since the last look asked for and did not get.
            const uint64_t wanted_at = pool.header.offset() + uint64_t(kLightPoolMostPagesWanted) * sizeof(uint32_t);
            uint32_t wanted = 0;
            arena->read_at(wanted_at, &wanted, sizeof(wanted));
            if (wanted != 0) {
                const uint32_t none = 0;
                arena->update_at(wanted_at, &none, sizeof(none));
            }
            uint32_t free_pages = 0;
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolFreePages) * sizeof(uint32_t), &free_pages, sizeof(free_pages));
            // Short for long without a frame asking for more: under a sixteenth free for
            // kLightPoolAheadFrames, which the sweep had at its shortest keep (velk_pieces_keep_frames)
            // to free what is out of view, so that what holds the pool is what is read. A quarter
            // more, then, before a burst finds it full; at most once every kLightPoolAheadEvery
            // frames.
            const bool low = free_pages * 16u < pool.pages();
            if (!low) {
                pool.low_since = 0;
            } else if (pool.low_since == 0) {
                pool.low_since = ctx.present_counter;
            } else if (wanted == 0 && ctx.present_counter >= pool.low_since + kLightPoolAheadFrames
                       && ctx.present_counter >= pool.ahead_frame + kLightPoolAheadEvery && pool.segments.size() < pool.max_segments) {
                pool.ahead_frame = ctx.present_counter;
                pool.low_since = 0;
                const uint32_t added = grow_light_pool(pool, *arena, (pool.pages() / 4u + kLightSegmentPages - 1u) / kLightSegmentPages);
                VELK_LOG(I, "light pool +%u to %u pg ahead: %u free for %u frames", added, pool.pages(), free_pages,
                         kLightPoolAheadFrames);
            }
            // Short only when frames still asked for pages that were not there a check after the
            // first that did: a frame asking makes the sweep give back what pieces out of view hold
            // (velk_pieces_keep_frames), which may be room enough.
            // Frames still in flight when it last grew asked before its new pages were on the
            // stack: the check after a growth does not count.
            const bool settling = ctx.present_counter < pool.grew_frame + 8u;
            if (!settling) {
                pool.short_checks = wanted != 0 ? pool.short_checks + 1u : 0u;
            }
            if (!settling && pool.short_checks >= 2u && pool.segments.size() < pool.max_segments) {
                pool.grew_frame = ctx.present_counter;
                // What was asked for, and a segment more. Still short at the next check, at least
                // half the pool again: a frame asks for at most its split list's worth
                // (VELK_LIGHT_SPLIT_CAPACITY), far less than a new view wants, so that the pool
                // reaches it in a few steps.
                uint32_t asked = wanted + kLightSegmentPages;
                if (pool.short_checks > 2u) {
                    asked = std::max(asked, pool.pages() / 2u);
                }
                const uint32_t added = grow_light_pool(pool, *arena, (asked + kLightSegmentPages - 1u) / kLightSegmentPages);
                VELK_LOG(I, "light pool +%u to %u pg: %u pages asked for were not there, %u checks in a row, %u free at the "
                            "last sweep",
                         added, pool.pages(), wanted, pool.short_checks, free_pages);
            }
        }
        // VELK_LIGHT_READ's pixels, every frame: the lighting's terms at each, before tonemapping,
        // with the frame that wrote them (a pixel nothing lit keeps an older frame's).
        if (pool.header.valid() && pool.reads && pool.debug.valid()) {
            uint32_t r[kLightReadPixels * (1u + kLightReadWords)] = {};
            arena->read_at(pool.debug.offset() + uint64_t(kLightDebugReadPixels) * sizeof(uint32_t), r, sizeof(r));
            // The pool's work around them: the last frame's asks for pieces, and since the log
            // last read them, pieces made and asks not made.
            uint32_t counts[4] = {};
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolLastSplits) * sizeof(uint32_t), &counts[0], sizeof(uint32_t));
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolSumPiecesMade) * sizeof(uint32_t), &counts[1], sizeof(uint32_t));
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolSumSplitsRefused) * sizeof(uint32_t), &counts[2], sizeof(uint32_t));
            arena->read_at(pool.header.offset() + uint64_t(kLightPoolLastUntracedPoints) * sizeof(uint32_t), &counts[3], sizeof(uint32_t));
            VELK_LOG(I, "light read pool (now %llu): last frame %u asks for pieces, %u untraced points listed; summed %u pieces made, "
                        "%u asks not made",
                     static_cast<unsigned long long>(ctx.present_counter), counts[0], counts[3], counts[1], counts[2]);
            // One frame's counts (the debug block's LAST_RAYS_CAST on), with the frame they are of
            // (READ_STAMP as the sweep copied it), the view by its state as `light submit` names it.
            uint32_t last[3] = {};
            arena->read_at(pool.debug.offset() + uint64_t(kLightDebugLastRaysCast) * sizeof(uint32_t), last, sizeof(last));
            VELK_LOG(I, "light frame %p,%u (now %llu): rays %u of the points; pending %u", static_cast<const void*>(&state),
                     last[2], static_cast<unsigned long long>(ctx.present_counter), last[0], last[1]);
            for (uint32_t k = 0; k < kLightReadPixels; ++k) {
                const uint32_t packed = r[k];
                if (packed == kLightNone) continue;
                float t[kLightReadWords];
                std::memcpy(t, r + kLightReadPixels + k * kLightReadWords, sizeof(t));
                uint32_t flags = 0;
                std::memcpy(&flags, t + 22, sizeof(flags));
                VELK_LOG(I, "light read %u,%u frame %u (now %llu): albedo %.3f %.3f %.3f, direct %.3f %.3f %.3f, indirect "
                            "%.3f %.3f %.3f, env %.3f %.3f %.3f, specular %.3f %.3f %.3f, area %.3f %.3f %.3f, colour %.3f %.3f "
                            "%.3f, metallic %.2f, points %s, probed %s, match %.2f",
                         packed & 0xFFFFu, packed >> 16u, flags >> 8u, static_cast<unsigned long long>(ctx.present_counter),
                         t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15],
                         t[16], t[17], t[18], t[19], t[20], t[21], (flags & 1u) ? "traced" : "not traced",
                         (flags & 2u) ? "yes" : "no", t[23]);
                {
                    const uint32_t* f = r + kLightReadPixels + k * kLightReadWords + 70u;
                    float through = 0.f;
                    float n_dot_l = 0.f;
                    float lit[3] = {};
                    float normal[3] = {};
                    std::memcpy(&through, f + 1, sizeof(through));
                    std::memcpy(&n_dot_l, f + 2, sizeof(n_dot_l));
                    std::memcpy(lit, f + 4, sizeof(lit));
                    std::memcpy(normal, f + 7, sizeof(normal));
                    VELK_LOG(I, "light read %u,%u first light %d: through %.3f, N.L %.3f; ids %08x; highlights %.3f %.3f %.3f; "
                                "normal %.3f %.3f %.3f",
                             packed & 0xFFFFu, packed >> 16u, f[0] == kLightNone ? -1 : static_cast<int>(f[0]), through, n_dot_l, f[3],
                             lit[0], lit[1], lit[2], normal[0], normal[1], normal[2]);
                }
                // What it reads of its triangle's pieces (velk_light_explain): the first light's
                // shade (0 lit to 15 blocked, - for none), traces and first slot's blocker code
                // (b: 1 at 1/128 or nearer, doubling each step, 13 at 32 or more, 15 nothing in the
                // way, 0 not traced) of each point, weights in 255ths.
                const uint32_t* e = r + kLightReadPixels + k * kLightReadWords + 24u;
                const auto shade = [](uint32_t word) { return word == kLightNone ? -1 : static_cast<int>(word & 15u); };
                const auto level = [&](uint32_t at, char* text, size_t size) {
                    std::snprintf(text, size, "%d/%u b%u %d/%u b%u %d/%u b%u w %u %u %u", shade(e[at]), e[at + 3] & 255u,
                                  (e[at + 3] >> 8u) & 15u, shade(e[at + 1]), e[at + 4] & 255u, (e[at + 4] >> 8u) & 15u,
                                  shade(e[at + 2]), e[at + 5] & 255u, (e[at + 5] >> 8u) & 15u, e[at + 6] & 255u,
                                  (e[at + 6] >> 8u) & 255u, (e[at + 6] >> 16u) & 255u);
                };
                char vertices[128];
                char read_level[128];
                level(7u, vertices, sizeof(vertices));
                level(14u, read_level, sizeof(read_level));
                // Listed and changed frames as their distance back from the pool's frame, - for none.
                const auto back = [&](uint32_t frame) {
                    return frame == kLightNone ? -1 : static_cast<int>(int64_t(e[27]) - int64_t(frame & 0x0FFFFFFFu));
                };
                VELK_LOG(I, "light read %u,%u frame %u: record %08x triangle %u of %u, coarse %08x, root %08x, reads %08x depth %u "
                            "(deepest %d) cell %u tri %u px; vertices %s listed -%d -%d -%d changed -%d; level %s listed -%d -%d -%d "
                            "changed -%d",
                         packed & 0xFFFFu, packed >> 16u, flags >> 8u, e[0], e[1], e[2], e[3], e[4], e[5], e[6] & 255u,
                         e[45] == kLightNone ? -1 : static_cast<int>(e[45]), (e[6] >> 8u) & 255u, e[6] >> 16u, vertices, back(e[21]),
                         back(e[22]), back(e[23]), back(e[28]), read_level, back(e[24]), back(e[25]), back(e[26]), back(e[29]));
                float place[15];
                std::memcpy(place, e + 30, sizeof(place));
                VELK_LOG(I, "light read %u,%u frame %u places: corners %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f, at %.3f "
                            "%.3f %.3f, light %.4f %.4f %.4f",
                         packed & 0xFFFFu, packed >> 16u, flags >> 8u, place[0], place[1], place[2], place[3], place[4], place[5],
                         place[6], place[7], place[8], place[9], place[10], place[11], place[12], place[13], place[14]);
            }
        }
        // What it holds, now and then, from the last frame's complete counts.
        if (pool.header.valid() && ctx.present_counter >= pool.log_frame + 30u) {
            pool.log_frame = ctx.present_counter;
            // The pool's header as it is now, by its words' names; the summed ones cleared after.
            uint32_t h[kLightPoolHeaderWords] = {};
            arena->read_at(pool.header.offset(), h, sizeof(h));
            const auto clear = [&](uint32_t word, uint32_t count) {
                const uint32_t zeros[16] = {};
                arena->update_at(pool.header.offset() + uint64_t(word) * sizeof(uint32_t), zeros, count * sizeof(uint32_t));
            };
            uint32_t written = 0;
            uint32_t took = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + uint64_t(kCutLastRewritten) * sizeof(uint32_t), words, sizeof(words));
                written += words[0];
                took += words[1];
            }
            clear(kLightPoolSumChanged, 1u);
            VELK_LOG(I, "light: %u pages, %u of them free; records %u paged, %u one value; last frame %u searched, %u one values "
                        "traced, points listed: %u untraced coarse, %u due coarse, %u fine; "
                        "%u records changed since the last log, %u records "
                        "chosen again of which %u kept their light, %u lit pixels untraced; budget %u rays",
                     h[kLightPoolPages], h[kLightPoolFreePages], h[kLightPoolLastRecordsPaged], h[kLightPoolLastRecordsOneValue],
                     h[kLightPoolLastSearched], h[kLightPoolLastTraces],
                     h[kLightPoolLastUntracedPoints], h[kLightPoolLastCoarsePoints], h[kLightPoolLastFinePoints],
                     h[kLightPoolSumChanged], written, took, h[kLightPoolLastUntracedPixels], h[kLightPoolRayBudget]);
            uint32_t longest = 0;
            uint32_t most = 0;
            for (const auto& c : state.cuts) {
                uint32_t words[2] = {};
                arena->read_at(c.region.offset() + uint64_t(kCutLastLongestWalk) * sizeof(uint32_t), words, sizeof(words));
                longest = std::max(longest, words[0]);
                most = std::max(most, words[1]);
            }
            VELK_LOG(I, "choosing: last frame the longest walk of one thread visited %u hierarchy nodes, the most "
                        "records one walk chose %u",
                     longest, most);
            // The light trace's worst frame since the last log: its rays, and their walks' steps.
            clear(kLightPoolWorstRays, kLightPoolWorstFirstRays + 1u - kLightPoolWorstRays);
            const uint32_t worst_rays = h[kLightPoolWorstRays];
            const uint32_t worst_steps = h[kLightPoolWorstSteps];
            clear(kLightPoolLongestWalk, 1u);
            VELK_LOG(I, "light trace: worst frame %u rays (%u for first traces), %u walk steps, %.0f steps a ray; longest walk "
                        "of one thread %u steps; GPU %.2f ms the last frame, %.2f ms at most",
                     worst_rays, h[kLightPoolWorstFirstRays], worst_steps,
                     worst_rays != 0 ? double(worst_steps) / double(worst_rays) : 0.0, h[kLightPoolLongestWalk],
                     double(pool.trace_ms), double(pool.most_trace_ms));
            pool.most_trace_ms = 0.f;
            // Since the last log: what a ray's walk tests, and which own points were traced.
            clear(kLightPoolSumTriangles, kLightPoolSumLaterTraces + 1u - kLightPoolSumTriangles);
            const double rays = h[kLightPoolSumRays] != 0 ? double(h[kLightPoolSumRays]) : 1.0;
            const double steps = double(h[kLightPoolSumSteps]) * 16.0;
            const double tris = double(h[kLightPoolSumTriangles]) * 16.0;
            const double tested = double(h[kLightPoolSumRecords]);
            clear(kLightPoolSumRecordsHit, 7u);
            VELK_LOG(I, "light trace: a ray tests %.0f triangles, %.0f records (%.1f through their box: %.1f patches, %.1f quads, "
                        "%.1f without an instance, %.1f clusters, %.1f whole meshes, %.1f meshes without a tree), %.0f nodes; "
                        "points traced %u first, %u later",
                     tris / rays, tested / rays, double(h[kLightPoolSumRecordsHit]) / rays, double(h[kLightPoolSumHitBy]) / rays,
                     double(h[kLightPoolSumHitBy + 1u]) / rays, double(h[kLightPoolSumHitBy + 2u]) / rays,
                     double(h[kLightPoolSumHitBy + 3u]) / rays, double(h[kLightPoolSumHitBy + 4u]) / rays,
                     double(h[kLightPoolSumHitBy + 5u]) / rays, std::max(steps - tris - tested, 0.0) / rays,
                     h[kLightPoolSumFirstTraces], h[kLightPoolSumLaterTraces]);
            clear(kLightPoolSumMeshWide, 8u);
            VELK_LOG(I, "light trace: whole meshes a ray walks by box size %.1f under 1 unit, %.1f under 4, %.1f under 16, %.1f "
                        "more; by triangles %.1f under 64, %.1f under 512, %.1f under 4096, %.1f more",
                     double(h[kLightPoolSumMeshWide]) / rays, double(h[kLightPoolSumMeshWide + 1u]) / rays,
                     double(h[kLightPoolSumMeshWide + 2u]) / rays, double(h[kLightPoolSumMeshWide + 3u]) / rays,
                     double(h[kLightPoolSumMeshTris]) / rays, double(h[kLightPoolSumMeshTris + 1u]) / rays,
                     double(h[kLightPoolSumMeshTris + 2u]) / rays, double(h[kLightPoolSumMeshTris + 3u]) / rays);
            // Since the last log: where traces come from, a change or the view.
            clear(kLightPoolSumRetraces, kLightPoolSumSplitsRefused + 1u - kLightPoolSumRetraces);
            clear(kLightPoolSumLaidInherited, 1u);
            VELK_LOG(I, "light causes: %u points traced again from none after a change; points laid out for records without light %u, "
                        "for records grown from one value %u, from a coarser record's %u",
                     h[kLightPoolSumRetraces], h[kLightPoolSumLaidNew], h[kLightPoolSumLaidFromOne], h[kLightPoolSumLaidInherited]);
            // Since the last log: records whose lights were chosen, and of them those that left out
            // a light at least a tenth, or a quarter, as strong as the strongest they took.
            // Since the last log, in every 16th frame: points listed by what listed them.
            clear(kLightPoolSumListedBy, 4u);
            VELK_LOG(I, "light listed (every 16th frame): %u points by records without pieces, by triangles in pieces %u under 8 "
                        "px on screen, %u under 64, %u larger",
                     h[kLightPoolSumListedBy], h[kLightPoolSumListedBy + 1u], h[kLightPoolSumListedBy + 2u],
                     h[kLightPoolSumListedBy + 3u]);
            clear(kLightPoolSumSlotChoices, 3u);
            VELK_LOG(I, "light slots: %u records chose their lights, %u left out one a tenth as strong as their strongest, %u "
                        "one a quarter as strong",
                     h[kLightPoolSumSlotChoices], h[kLightPoolSumLeftOutTenth], h[kLightPoolSumLeftOutQuarter]);
            // Pieces: those kept at the last frame's mark and the deepest of them, and since the
            // last log those made, freed and asked for but not made; and why pieces were let go
            // of (unread, or their parent's light), the rest of those freed going with their record.
            VELK_LOG(I, "light pieces: %u kept, the deepest at %u; last frame %u asks; since the last log %u made, %u freed "
                        "(%u unread, %u with their parent's light), %u asks not made",
                     h[kLightPoolLastMarkPieces], h[kLightPoolLastMarkDeepest], h[kLightPoolLastSplits], h[kLightPoolSumPiecesMade],
                     h[kLightPoolSumPiecesFreed], h[kLightPoolSumPiecesUnread], h[kLightPoolSumPiecesMerged],
                     h[kLightPoolSumSplitsRefused]);
            clear(kLightPoolSumPiecesUnread, 2u);
            // What the culling arena holds: in use of its buffer, and this view's part of it.
            uint64_t cut_bytes = 0;
            for (const auto& c : state.cuts) cut_bytes += c.region.size();
            uint64_t pool_bytes = pool.header.size();
            for (const auto& s : pool.segments) pool_bytes += s.size();
            const uint64_t view_other = state.together.size() + state.item_descriptor_region.size() + state.retest.size()
                                      + state.quads.size() + state.ray_top.size() + state.rename_words.size();
            VELK_LOG(I, "culling arena: %.1f of %.1f MB in use; this view's light pool %.1f MB, cut stores %.1f MB, other "
                        "%.1f MB",
                     double(arena->capacity() - arena->free_bytes()) / 1e6, double(arena->capacity()) / 1e6,
                     double(pool_bytes) / 1e6, double(cut_bytes) / 1e6, double(view_other) / 1e6);
            // The ray lists: records rays trace against all records, from the partitions' states.
            uint32_t traced_records = 0;
            uint32_t all_records = 0;
            uint32_t most_traced = 0;
            uint32_t unsorted = 0;
            uint32_t balanced = 0;
            uint32_t tried_most = 0;
            uint32_t limit_least = 0xFFFFFFFFu;
            for (const auto& c : state.cuts) {
                uint32_t header[kCutHeaderWords] = {};
                arena->read_at(c.region.offset(), header, sizeof(header));
                vector<uint32_t> states(size_t(c.partitions) * kCutPartitionWords);
                arena->read_at(uint64_t(header[kCutStatesWord]) * sizeof(uint32_t), states.data(), states.size() * sizeof(uint32_t));
                for (uint32_t p = 0; p < c.partitions; ++p) {
                    const uint32_t ray_count = states[size_t(p) * kCutPartitionWords + kPartRayCount];
                    traced_records += ray_count;
                    all_records += states[size_t(p) * kCutPartitionWords + kPartCount];
                    most_traced = std::max(most_traced, ray_count);
                    unsorted += ray_count > kRaySortMax ? 1u : 0u;
                    const uint32_t tried = states[size_t(p) * kCutPartitionWords + kPartBalanced];
                    if (ray_count > 4u && tried != 0u) {
                        ++balanced;
                        tried_most = std::max(tried_most, tried);
                        // As velk_lbvh_build_bounded: a balanced tree's height and 16 more.
                        const uint32_t leaves = (ray_count + 3u) / 4u;
                        uint32_t bits = 0;
                        while (leaves > 1u && (1u << bits) < leaves) ++bits;
                        limit_least = std::min(limit_least, bits + 16u);
                    }
                }
            }
            VELK_LOG(I, "ray lists: %u of %u records; at most %u in a partition, %u partitions past %u sorted in their pages, "
                        "%u trees balanced for being too deep sorted (sorted heights up to %u, limits from %u)",
                     traced_records, all_records, most_traced, unsorted, kRaySortMax, balanced, tried_most,
                     balanced != 0u ? limit_least : 0u);
            // The deepest the walk's stack got since the last log, in the trace and the probes.
            clear(kLightPoolDeepestTrace, 2u);
            VELK_LOG(I, "walk stack: deepest %u in the light trace, %u in the probes", h[kLightPoolDeepestTrace],
                     h[kLightPoolDeepestProbes]);
            // What the screen probes did with their cells since the last log.
            clear(kLightPoolSumProbesRead, 4u);
            VELK_LOG(I, "probes: %u read their cell without rays, %u added rays to it, %u cast rays alone; %u settled cells woken",
                     h[kLightPoolSumProbesRead], h[kLightPoolSumProbesRead + 1u], h[kLightPoolSumProbesRead + 2u],
                     h[kLightPoolSumProbesRead + 3u]);
            // Where a ray's walk spends its steps, since the last log (the same rays as above).
            clear(kLightPoolSumTopNodes, kLightPoolSumPartitions + 1u - kLightPoolSumTopNodes);
            VELK_LOG(I, "light trace: a ray walks %.0f top tree nodes, enters %.1f partitions, walks %.0f of their tree nodes "
                        "and %.0f mesh tree nodes",
                     double(h[kLightPoolSumTopNodes]) * 16.0 / rays, double(h[kLightPoolSumPartitions]) / rays,
                     double(h[kLightPoolSumPartitionNodes]) * 16.0 / rays, double(h[kLightPoolSumMeshNodes]) * 16.0 / rays);
            // The top tree's partitions: how many have records for rays, and how much their boxes
            // overlap, as the sum of their volumes over the volume of the box around them all.
            if (state.ray_top.valid()) {
                vector<uint32_t> top(size_t(state.ray_top.size() / sizeof(uint32_t)));
                arena->read_at(state.ray_top.offset(), top.data(), top.size() * sizeof(uint32_t));
                const uint32_t cut_count = top[0];
                const uint32_t leaves = top[1];
                const uint32_t node_count = top[2];
                const size_t nodes_at = kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * kRayTopLeafWords;
                double volume = 0.0;
                double lo[3] = {1e30, 1e30, 1e30};
                double hi[3] = {-1e30, -1e30, -1e30};
                uint32_t boxes = 0;
                for (uint32_t n = 0; n < node_count && nodes_at + (size_t(n) + 1u) * kCutNodeWords <= top.size(); ++n) {
                    const uint32_t* node = top.data() + nodes_at + size_t(n) * kCutNodeWords;
                    for (uint32_t k = 0; k < 2; ++k) {
                        if ((node[12u + k] & kCutLeaf) == 0 || node[12u + k] == 0xFFFFFFFFu) continue;
                        float b[6];
                        std::memcpy(b, node + k * 6u, sizeof(b));
                        if (b[0] > b[3] || b[1] > b[4] || b[2] > b[5]) continue;
                        volume += double(b[3] - b[0]) * double(b[4] - b[1]) * double(b[5] - b[2]);
                        for (int a = 0; a < 3; ++a) {
                            lo[a] = std::min(lo[a], double(b[a]));
                            hi[a] = std::max(hi[a], double(b[3 + a]));
                        }
                        ++boxes;
                    }
                }
                const double around = boxes != 0 ? (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]) : 0.0;
                VELK_LOG(I, "ray top tree: %u partitions with records for rays of %u, their boxes' volumes %.1f times the "
                            "box around them all (%.0f by %.0f by %.0f)",
                         boxes, leaves, around > 0.0 ? volume / around : 0.0, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
            }
        }
    }
    record = record || state.light_pool.pages() != state.light_pool.recorded_pages;
    state.light_pool.recorded_pages = state.light_pool.pages();

    // The top tree over the kept cuts rays walk: when what it holds changes, a new region with
    // room for its header, its cuts, its leaves and its nodes, which emit_instance_cull_pass
    // writes. A new one, since earlier frames may still read the old.
    const uint64_t ray_top_layout = instance_ray_top_layout(state, plain_cut, plain_count);
    if (!plain_cut) {
        state.ray_top = {};
        state.ray_top_layout = 0;
        state.ray_top_fresh = false;
    } else if (!state.ray_top.valid() || ray_top_layout != state.ray_top_layout) {
        const uint32_t cut_count = plain_cut ? 1u : 0u;
        const uint32_t leaves = plain_cut ? plain_count : 0u;
        const uint64_t words = uint64_t(kRayTopHeaderWords) + uint64_t(cut_count) * kRayTopCutWords + uint64_t(leaves) * kRayTopLeafWords
                             + uint64_t(top_tree_nodes(leaves)) * kCutNodeWords;
        state.ray_top = arena->alloc(words * sizeof(uint32_t));
        state.ray_top_layout = ray_top_layout;
        state.ray_top_fresh = state.ray_top.valid();
        // The passes name its word.
        record = true;
    }

    state.reserved = true;
    state.reserved_record = record;
    state.reserved_plain_cut = plain_cut;
    state.reserved_light_pool_word = state.light_pool.segments.empty() ? kCullNoSlot : state.light_pool.word();
}

/**
 * Records a view's culling as reserve_instance_cull worked it out this frame; nothing when it
 * did not. Allocates nothing, so the culling arena's buffer it names stays the frame's.
 */
inline void emit_instance_cull_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                    uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph,
                                    uint32_t pyramid_word = kCullNoSlot, bool whole_cut = false,
                                    uint32_t plain_table_word = kCullNoSlot, uint32_t plain_count = 0)
{
    VELK_PERF_SCOPE("instance_cull.emit");
    if (!state.reserved || !batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }
    state.reserved = false;
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) {
        return;
    }
    bool record = state.reserved_record;
    const bool plain_cut = state.reserved_plain_cut;
    const uint32_t light_pool_word = state.reserved_light_pool_word;
    const auto cut_word = [&](const void* key) {
        for (const auto& c : state.cuts) {
            if (c.key == key) {
                return static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
            }
        }
        return kCullNoSlot;
    };

    IGpuBuffer* arena_buffer = arena->buffer();
    if (!arena_buffer) {
        return;
    }
    // The copies below name the arena's buffer, which growing it replaces.
    // A grown arena's new buffer can have the old one's address, so its size counts too.
    if (arena_buffer != state.arena_buffer || arena_buffer->size_bytes() != state.arena_size) {
        record = true;
        ++state.arena_generation;
    }
    state.arena_buffer = arena_buffer;
    state.arena_size = arena_buffer->size_bytes();
    if (state.retest_capacity == 0) {
        pyramid_word = kCullNoSlot;
    }
    record = record || pyramid_word != state.pyramid_word;
    state.pyramid_word = pyramid_word;
    if (state.tiny_word == kCullNoSlot || state.tiny_capacity == 0) {
        state.tiny_word = kCullNoSlot;
        state.tiny_pixels = 0.f;
    }
    record = record || state.tiny_word != state.recorded_tiny_word || state.tiny_pixels != state.recorded_tiny_pixels
          || whole_cut != state.recorded_whole_cut;
    state.recorded_whole_cut = whole_cut;
    state.recorded_tiny_word = state.tiny_word;
    state.recorded_tiny_pixels = state.tiny_pixels;
    state.retest_dirty = state.retest_dirty || record;

    // A mesh's count of visible slots, and at 7 words on, those the second phase added.
    const uint32_t zero = 0;
    for (uint32_t word : state.count_words) {
        arena->update_at(uint64_t(word) * sizeof(uint32_t), &zero, sizeof(zero));
        arena->update_at(uint64_t(word + 7u) * sizeof(uint32_t), &zero, sizeof(zero));
    }

    // All the stages are recorded together, each into its own pass.
    for (const auto& pass : state.passes) record = record || !pass;
    CachedPassRecording stage_recs[kCullStages];
    const auto record_all = [&]() {
        CachedPassRecording& rec = stage_recs[kCullStages - 1u];
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_cull_compute_src, 0x4375'6c6c'496e'7374ULL);
        auto partition_pipeline = pipeline_for(instance_partition_cull_src, 0x4375'6c6c'5061'7274ULL);
        auto runs_pipeline = pipeline_for(instance_cluster_runs_src, 0x4375'6c6c'5275'6e73ULL);
        auto fill_pipeline = pipeline_for(instance_cluster_fill_src, 0x4375'6c6c'4669'6c6cULL);
        auto begin_pipeline = pipeline_for(instance_cut_begin_src, 0x4375'7442'6567'696eULL);
        auto pages_pipeline = pipeline_for(instance_cut_pages_src, 0x4375'7450'6167'6573ULL);
        auto choose_pipeline = pipeline_for(instance_cut_choose_src, 0x4375'7443'686f'6f73ULL);
        auto place_pipeline = pipeline_for(instance_cut_place_src, 0x4375'7450'6c61'6365ULL);
        auto draw_pipeline = pipeline_for(instance_cut_draw_src, 0x4375'7444'7261'7720ULL);
        auto visible_pipeline = pipeline_for(instance_cut_visible_src, 0x4375'7456'6973'6962ULL);
        auto tree_pipeline = pipeline_for(instance_cut_tree_src, 0x4375'7454'7265'6520ULL);
        auto top_pipeline = pipeline_for(instance_cut_top_src, 0x4375'7454'6f70'2020ULL);
        auto large_pipeline = pipeline_for(instance_cut_large_src, 0x4375'744c'6172'6765ULL);
        auto mark_pipeline = pipeline_for(light_mark_src, 0x4c69'6768'744d'726bULL);
        auto sweep_pipeline = pipeline_for(light_sweep_src, 0x4c69'6768'7453'7770ULL);
        auto pieces_pipeline = pipeline_for(light_pieces_src, 0x4c69'6768'7450'6363ULL);
        auto rename_pipeline = pipeline_for(instance_cut_rename_src, 0x4375'7452'656e'616dULL);
        auto items_pipeline = pipeline_for(instance_items_src, 0x4375'6c6c'4974'656dULL);
        auto stale_pipeline = pipeline_for(instance_cut_stale_src, 0x4375'7453'7461'6c65ULL);
        const bool have_pipelines = pipeline && partition_pipeline && runs_pipeline && fill_pipeline && begin_pipeline
                                 && pages_pipeline && choose_pipeline && place_pipeline && draw_pipeline
                                 && visible_pipeline && tree_pipeline && top_pipeline && large_pipeline && mark_pipeline && sweep_pipeline && pieces_pipeline
                                 && rename_pipeline
                                 && items_pipeline && stale_pipeline;
        IGpuCommandBuffer::Ptr stage_cmds[kCullStages];
        for (auto& stage_cmd : stage_cmds) {
            stage_cmd = have_pipelines ? ctx.backend->create_command_buffer() : nullptr;
            if (!stage_cmd) {
                return;
            }
        }
        // The stage being recorded; next_stage ends it and begins the next.
        uint32_t stage = 0;
        IGpuCommandBuffer* cmd = stage_cmds[0].get();
        const auto begin_stage = [&]() {
            cmd = stage_cmds[stage].get();
            cmd->begin_recording();
            cmd->push_label(kCullStageNames[stage]);
            // Earlier frames may still be drawing from these lists, and the stage before
            // wrote what this one reads.
            cmd->record_global_barrier();
        };
        const auto next_stage = [&]() {
            cmd->pop_label();
            cmd->end_recording();
            ++stage;
            begin_stage();
        };
        begin_stage();

        const bool together = state.together_items != 0;
        const auto together_word = together ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
        const uint32_t together_entries_word =
            together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords;
        const uint32_t together_items_word = together_entries_word + state.together_entries;
        if (together) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(together_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        // The records of items drawn by their own pass: none unless the draw pass finds one.
        const uint32_t item_records_word = instance_item_records_word(state);
        if (item_records_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(item_records_word) * sizeof(uint32_t),
                                     uint64_t(state.together_items) * sizeof(uint32_t));
        }
        if (state.retest.valid()) {
            cmd->record_clear_buffer(arena_buffer, state.retest.offset(), sizeof(uint32_t));
        }
        // Only the count: the rest of the header holds what the last frame drew until
        // instance_tiny_args_src writes it again.
        if (state.tiny_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(state.tiny_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        // The quad list's instance count.
        const uint32_t quads_word = instance_quad_word(state);
        if (quads_word != kCullNoSlot) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + kQuadListCount) * sizeof(uint32_t), sizeof(uint32_t));
            cmd->record_clear_buffer(arena_buffer, uint64_t(quads_word + kQuadListInCuts) * sizeof(uint32_t), sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        const auto dispatch =[&](IGpuPipeline* used, const void* pc, uint32_t pc_size, uint32_t groups,
                                  uint32_t args_word = kCullNoSlot) {
            DispatchCall dc{};
            dc.pipeline = used;
            dc.groups_x = groups;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = pc_size;
            std::memcpy(dc.root_constants, pc, pc_size);
            if (args_word != kCullNoSlot) {
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(args_word) * sizeof(uint32_t));
            } else {
                cmd->record_dispatch(dc);
            }
        };

        // The instanced visuals' items, from their slot tables, before anything reads them.
        if (together && state.instanced_items != 0 && state.item_descriptor_region.valid()) {
            const uint32_t objects_word = together_items_word + state.together_items * kPlainItemWords;
            const uint32_t items_pc[7] = {static_cast<uint32_t>(state.item_descriptor_region.offset() / sizeof(uint32_t)),
                                          state.item_descriptor_count, together_items_word, state.instanced_first_item,
                                          state.instanced_items, objects_word, objects_word + state.together_items};
            dispatch(items_pipeline.get(), items_pc, sizeof(items_pc), (state.instanced_items + 63u) / 64u);
            cmd->record_global_barrier();
        }

        // The kept cut: the scene's objects', by item.
        struct CutJob
        {
            uint32_t cut;
            uint32_t partition_table;
            uint32_t partition_count;
            float max_distance;
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t items_word;  ///< The items.
            float pad;            ///< What its tree's boxes grow by besides each item's own: see instance_cut_tree_src.
        };
        const uint32_t together_objects_word = together_items_word + state.together_items * kPlainItemWords;
        const uint32_t together_next_word = together_objects_word + state.together_items;
        const uint32_t together_head_word = together_next_word + state.together_items;
        vector<CutJob> jobs;
        const uint32_t plain_cut_word = plain_cut ? cut_word(&kPlainCutKey) : kCullNoSlot;
        if (plain_cut_word != kCullNoSlot) {
            jobs.push_back({plain_cut_word, plain_table_word, plain_count, 0.f, 0u, 0u, 0u, together_word, state.together_chosen,
                            together_items_word, 0.f});
        }
        for (const CutJob& job : jobs) {
            dispatch(begin_pipeline.get(), &job.cut, sizeof(job.cut), 1u);
        }
        cmd->record_global_barrier();

        // Records named in batches made again this frame, before anything reads them.
        if (!state.renames.empty()) {
            for (const auto& rename : state.renames) {
                for (const auto& c : state.cuts) {
                    if (c.key != rename.key) continue;
                    const uint32_t pc[7] = {static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)), rename.pairs_word,
                                            static_cast<uint32_t>(rename.pairs.size() / 2u), rename.items_word,
                                            static_cast<uint32_t>(rename.items.size()), rename.blocks_word,
                                            static_cast<uint32_t>(rename.blocks.size() / 3u)};
                    dispatch(rename_pipeline.get(), pc, sizeof(pc), (c.capacity + 63u) / 64u);
                }
            }
            cmd->record_global_barrier();
            // Partitions with records that did not carry over choose again.
            for (const auto& rename : state.renames) {
                for (const auto& c : state.cuts) {
                    if (c.key != rename.key || c.partitions == 0) continue;
                    const uint32_t cut_pc = static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t));
                    dispatch(stale_pipeline.get(), &cut_pc, sizeof(cut_pc), c.partitions);
                }
            }
            cmd->record_global_barrier();
        }

        // Partitions next: whether they keep their records.
        struct PartitionPushC
        {
            uint32_t table_word;
            uint32_t entry_count;
            uint32_t globals_base;
            uint32_t cut_word;
            float refresh;
        };
        for (const CutJob& job : jobs) {
            const PartitionPushC pc{job.partition_table, job.partition_count, view_globals_base, job.cut, kCutRefresh};
            dispatch(partition_pipeline.get(), &pc, sizeof(pc), (job.partition_count + 63u) / 64u);
        }
        next_stage();

        // The kept cuts: records counted for the partitions that choose again, placed, written,
        // then drawn from; those that keep theirs keep their pages.
        if (!jobs.empty()) {
            struct ChoosePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t slot_count;
                uint32_t table_word;
                uint32_t globals_base;
                float lod_max_pixels;
                uint32_t write;
                uint32_t items_word;
                uint32_t objects_word;
                uint32_t next_word;
                uint32_t head_word;
                uint32_t object_slots;
                uint32_t partition_table;
                float quad_pixels;
                float light_pixels;
                uint32_t descriptors_word;
                uint32_t descriptor_count;
            };
            const auto choose = [&](const CutJob& job, uint32_t write) {
                const uint32_t partition_table = job.partition_table;
                const ChoosePushC pc{job.cut, job.instances_base, job.slot_count, job.table_word, view_globals_base,
                                     kLodMaxErrorPixels, write, job.items_word, together_objects_word, together_next_word,
                                     together_head_word, state.together_object_slots, partition_table, kQuadPixels,
                                     kLodMaxErrorPixels * kLightPixels / kLodElementPixels,
                                     state.item_descriptor_region.valid()
                                         ? static_cast<uint32_t>(state.item_descriptor_region.offset() / sizeof(uint32_t))
                                         : 0u,
                                     state.item_descriptor_region.valid() ? state.item_descriptor_count : 0u};
                dispatch(choose_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + kCutChooseGroups);
            };
            for (const CutJob& job : jobs) {
                choose(job, 0u);
            }
            next_stage();
            for (const CutJob& job : jobs) {
                dispatch(place_pipeline.get(), &job.cut, sizeof(job.cut), (job.partition_count + 63u) / 64u);
            }
            next_stage();
            for (const CutJob& job : jobs) {
                choose(job, 1u);
            }
            next_stage();
            // Then the trees for rays over the partitions that chose again: those with ray lists
            // past kRaySortMax that the cut's sort of long lists has room for are queued for it,
            // which starts each frame empty, and built after it.
            struct TreePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t items_word;
                float pad;
                uint32_t globals_base;
                float light_pixels;
                uint32_t mode;
            };
            const auto large_word = [&](const CutJob& job) {
                for (const auto& c : state.cuts) {
                    if (static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)) == job.cut) return c.large_word;
                }
                return kCullNoSlot;
            };
            for (const CutJob& job : jobs) {
                const uint32_t large = large_word(job);
                if (large != kCullNoSlot) {
                    cmd->record_clear_buffer(arena_buffer, uint64_t(large + kCutLargeLists) * sizeof(uint32_t), 2u * sizeof(uint32_t));
                }
            }
            cmd->record_global_barrier();
            const auto tree = [&](const CutJob& job, uint32_t mode, uint32_t args_word) {
                const TreePushC pc{job.cut, job.instances_base, job.items_word, job.pad, view_globals_base,
                                   kLodMaxErrorPixels * kLightPixels / kLodElementPixels, mode};
                dispatch(tree_pipeline.get(), &pc, sizeof(pc), job.partition_count, args_word);
            };
            for (const CutJob& job : jobs) {
                tree(job, 0u, kCullNoSlot);
            }
            cmd->record_global_barrier();
            // The long lists' sort, its passes sized from its queue, then their trees.
            struct LargePushC
            {
                uint32_t cut_word;
                uint32_t instances_base;
                uint32_t items_word;
                float pad;
                uint32_t mode;
                uint32_t shift;
                uint32_t source;
            };
            const auto large_pass = [&](uint32_t mode, uint32_t shift, uint32_t source, uint32_t args) {
                for (const CutJob& job : jobs) {
                    const uint32_t large = large_word(job);
                    if (large == kCullNoSlot) continue;
                    const LargePushC pc{job.cut, job.instances_base, job.items_word, job.pad, mode, shift, source};
                    dispatch(large_pipeline.get(), &pc, sizeof(pc), 1u, args == kCullNoSlot ? kCullNoSlot : large + args);
                }
                cmd->record_global_barrier();
            };
            large_pass(0u, 0u, 0u, kCullNoSlot);
            large_pass(1u, 0u, 0u, kCutLargeBlockGroups);
            for (uint32_t digit = 0; digit < 4u; ++digit) {
                large_pass(2u, digit * 8u, digit & 1u, kCutLargeBlockGroups);
                large_pass(3u, digit * 8u, digit & 1u, kCutLargeListGroups);
                large_pass(4u, digit * 8u, digit & 1u, kCutLargeBlockGroups);
            }
            large_pass(5u, 0u, 0u, kCutLargeBlockGroups);
            large_pass(6u, 0u, 0u, kCutLargeBlockGroups);
            for (const CutJob& job : jobs) {
                const uint32_t large = large_word(job);
                if (large != kCullNoSlot) tree(job, 1u, large + kCutLargeListGroups);
            }
            cmd->record_global_barrier();

            // And the top tree over them all, its cuts and leaves written into the region
            // reserve_instance_cull made when they changed (instance_ray_top_layout).
            uint32_t leaves = 0;
            for (const CutJob& job : jobs) {
                leaves += job.partition_count;
            }
            if (state.ray_top.valid() && state.ray_top_fresh) {
                state.ray_top_fresh = false;
                const auto cut_count = static_cast<uint32_t>(jobs.size());
                const uint32_t node_count = top_tree_nodes(leaves);
                vector<uint32_t> words;
                words.resize(size_t(kRayTopHeaderWords) + size_t(cut_count) * kRayTopCutWords + size_t(leaves) * kRayTopLeafWords, 0u);
                words[kRayTopCuts] = cut_count;
                words[kRayTopLeaves] = leaves;
                words[kRayTopNodes] = node_count;
                words[kRayTopRoot] = node_count - 1u;
                uint32_t leaf = 0;
                for (uint32_t c = 0; c < cut_count; ++c) {
                    const CutJob& job = jobs[c];
                    uint32_t* cut = words.data() + kRayTopHeaderWords + size_t(c) * kRayTopCutWords;
                    cut[kRayTopCutCut] = job.cut;
                    cut[kRayTopCutInstancesBase] = job.instances_base;
                    cut[kRayTopCutItems] = job.items_word;
                    cut[kRayTopCutPartitions] = job.partition_count;
                    cut[kRayTopCutFirstLeaf] = leaf;
                    cut[kRayTopCutMoves] = state.items_move ? 1u : 0u;
                    cut[kRayTopCutQuads] = quads_word;
                    for (uint32_t p = 0; p < job.partition_count; ++p, ++leaf) {
                        uint32_t* at = words.data() + kRayTopHeaderWords + size_t(cut_count) * kRayTopCutWords + size_t(leaf) * kRayTopLeafWords;
                        at[kRayTopLeafCut] = c;
                        at[kRayTopLeafPartition] = p;
                    }
                }
                arena->write_at(state.ray_top.offset(), words.data(), words.size() * sizeof(uint32_t));
                state.ray_top_cuts.clear();
                for (const CutJob& job : jobs) state.ray_top_cuts.push_back(job.cut);
            }
            if (state.ray_top.valid()) {
                const uint32_t top_word = instance_ray_top_word(state);
                dispatch(top_pipeline.get(), &top_word, sizeof(top_word), 1u);
            }
            next_stage();

            // The pages no page table names are free again, for the next frame's choosing, and
            // the passes over the pools' records go up to the last page in use.
            for (const CutJob& job : jobs) {
                uint32_t pool_pages = 0;
                for (const auto& c : state.cuts) {
                    if (static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)) == job.cut) pool_pages = c.capacity / kCutPageRecords;
                }
                const uint32_t mark_pc[2] = {job.cut, 0u};
                dispatch(pages_pipeline.get(), mark_pc, sizeof(mark_pc), (pool_pages + 63u) / 64u + 1u);
            }
            cmd->record_global_barrier();
            for (const CutJob& job : jobs) {
                uint32_t pool_pages = 0;
                for (const auto& c : state.cuts) {
                    if (static_cast<uint32_t>(c.region.offset() / sizeof(uint32_t)) == job.cut) pool_pages = c.capacity / kCutPageRecords;
                }
                const uint32_t sweep_pc[2] = {job.cut, 1u};
                dispatch(pages_pipeline.get(), sweep_pc, sizeof(sweep_pc), (pool_pages / 32u + 1u + 63u) / 64u);
            }
            cmd->record_global_barrier();

            // The records are what they are this frame: the light pages none of them names are
            // free again.
            if (light_pool_word != kCullNoSlot) {
                const uint32_t begin_pc[2] = {light_pool_word, 1u};
                dispatch(sweep_pipeline.get(), begin_pc, sizeof(begin_pc), 1u);
                cmd->record_global_barrier();
                for (const CutJob& job : jobs) {
                    const uint32_t mark_pc[5] = {job.cut, light_pool_word, job.instances_base, job.items_word, view_globals_base};
                    dispatch(mark_pipeline.get(), mark_pc, sizeof(mark_pc), 1u, job.cut + kCutDrawGroups);
                }
                cmd->record_global_barrier();
                // The pieces: what no pixel reads unlinked, then those still linked marked.
                const uint32_t piece_groups = (state.light_pool.pages() + 63u) / 64u;
                const uint32_t unlink_pc[2] = {light_pool_word, 0u};
                dispatch(pieces_pipeline.get(), unlink_pc, sizeof(unlink_pc), piece_groups);
                cmd->record_global_barrier();
                const uint32_t keep_pc[2] = {light_pool_word, 1u};
                dispatch(pieces_pipeline.get(), keep_pc, sizeof(keep_pc), piece_groups);
                cmd->record_global_barrier();
                const uint32_t sweep_pc[2] = {light_pool_word, 0u};
                dispatch(sweep_pipeline.get(), sweep_pc, sizeof(sweep_pc), (state.light_pool.pages() / 32u + 63u) / 64u);
            }
            next_stage();
        } else {
            state.ray_top = {};
            state.ray_top_layout = 0;
            state.ray_top_cuts.clear();
            // No kept cuts: their stages are empty.
            while (stage + 1u < kCullStages) next_stage();
        }

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            float lod_max_pixels;
            uint32_t items_word;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t pyramid_word;
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
        };
        const auto retest_word = static_cast<uint32_t>(state.retest.offset() / sizeof(uint32_t));
        struct DrawPushC
        {
            uint32_t cut_word;
            uint32_t instances_base;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            float lod_max_pixels;
            uint32_t chosen_word;
            uint32_t chosen_capacity;
            uint32_t pyramid_word;
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
            uint32_t whole;
            uint32_t items_word;
            uint32_t quads_word;
            uint32_t quad_capacity;
            float quad_pad;
            uint32_t item_records_word;
        };
        // The records of partitions in view, in runs, then those drawn.
        for (const CutJob& job : jobs) {
            if (job.partition_count == 0) continue;
            const uint32_t visible_pc[4] = {job.cut, view_globals_base, whole_cut ? 1u : 0u, quads_word};
            dispatch(visible_pipeline.get(), visible_pc, sizeof(visible_pc), (job.partition_count + 63u) / 64u);
        }
        if (!jobs.empty()) cmd->record_global_barrier();
        for (const CutJob& job : jobs) {
            const DrawPushC pc{job.cut, job.instances_base, job.table_word, view_globals_base, job.max_distance,
                               kLodMaxErrorPixels, job.chosen_word, job.chosen_capacity,
                               pyramid_word, retest_word, state.retest_capacity,
                               state.tiny_word, state.tiny_capacity, state.tiny_pixels, whole_cut ? 1u : 0u, job.items_word,
                               quads_word, state.quad_capacity, job.pad, item_records_word};
            dispatch(draw_pipeline.get(), &pc, sizeof(pc), 1u, job.cut + kCutVisibleGroups);
        }
        // Without the scene's plain partitions, plain objects are chosen every frame.
        if (together && plain_cut_word == kCullNoSlot) {
            const PushC pc{0u, state.together_items, 0u, view_globals_base, 0.f, kLodMaxErrorPixels,
                           together_items_word, together_word, state.together_chosen,
                           pyramid_word, retest_word, state.retest_capacity,
                           state.tiny_word, state.tiny_capacity, state.tiny_pixels};
            dispatch(pipeline.get(), &pc, sizeof(pc), (state.together_items + 63u) / 64u);
        }
        cmd->record_global_barrier();

        // Draws by cluster: lay out each one's runs, then fill them.
        record_cluster_layout(*cmd, state, runs_pipeline.get(), fill_pipeline.get(), arena_buffer, together_word,
                              together_entries_word, false);

        // Each other batch's indirect args take the count of its list.
        constexpr uint64_t kInstanceCountOffset = BatchBufferLayout::kArgsOffset + sizeof(uint32_t);
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cluster_count != 0) {
                continue;
            }
            auto* entry = batch->storage_buffer() ? ctx.resources->find_buffer(batch->storage_buffer()) : nullptr;
            auto args = entry ? entry->buffer.lock() : nullptr;
            if (args) {
                cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * sizeof(uint32_t), args.get(),
                                        kInstanceCountOffset, sizeof(uint32_t));
            }
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        for (uint32_t k = 0; k < kCullStages; ++k) stage_recs[k].cmd = std::move(stage_cmds[k]);
        // The last stage holds the pipelines for all of them.
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(partition_pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
        rec.held.push_back(std::move(begin_pipeline));
        rec.held.push_back(std::move(pages_pipeline));
        rec.held.push_back(std::move(choose_pipeline));
        rec.held.push_back(std::move(place_pipeline));
        rec.held.push_back(std::move(tree_pipeline));
        rec.held.push_back(std::move(top_pipeline));
        rec.held.push_back(std::move(draw_pipeline));
        rec.held.push_back(std::move(visible_pipeline));
        rec.held.push_back(std::move(mark_pipeline));
        rec.held.push_back(std::move(sweep_pipeline));
        rec.held.push_back(std::move(pieces_pipeline));
        rec.held.push_back(std::move(rename_pipeline));
        rec.held.push_back(std::move(items_pipeline));
        rec.held.push_back(std::move(stale_pipeline));
    };
    if (record) record_all();
    const char* const* names = state.chosen_by_other ? kCullOtherStageNames : kCullStageNames;
    for (uint32_t k = 0; k < kCullStages; ++k) {
        bool stage_dirty = record;
        emit_cached_view_pass(state.passes[k], stage_dirty, names[k], graph,
                              [&](CachedPassRecording& rec) { rec = std::move(stage_recs[k]); });
    }
}

/**
 * @brief Adds the second phase of culling to @p graph, after what passed the first has been
 *        drawn and @p state's pyramid built from it. Only meaningful after
 *        emit_instance_cull_pass was given a pyramid.
 */
inline void emit_instance_retest_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                      uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph)
{
    if (!batches || state.pyramid_word == kCullNoSlot || !state.retest.valid() || !ctx.backend || !ctx.render_ctx) {
        return;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    IGpuBuffer* arena_buffer = arena ? arena->buffer() : nullptr;
    if (!arena_buffer) {
        return;
    }

    const char* name = state.chosen_by_other ? "other:instance.retest" : "instance.retest";
    emit_cached_view_pass(state.retest_pass, state.retest_dirty, name, graph, [&](CachedPassRecording& rec) {
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_retest_compute_src, 0x4375'6c6c'5265'7473ULL);
        auto runs_pipeline = pipeline_for(instance_cluster_runs_src, 0x4375'6c6c'5275'6e73ULL);
        auto fill_pipeline = pipeline_for(instance_cluster_fill_src, 0x4375'6c6c'4669'6c6cULL);
        auto cmd = pipeline && runs_pipeline && fill_pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        const bool together = state.together_items != 0;
        const auto together_word = together ? static_cast<uint32_t>(state.together.offset() / sizeof(uint32_t)) : 0u;
        const uint32_t together_entries_word =
            together_word + kCullChosenHeaderWords + state.together_chosen * kCullChosenRecordWords;

        cmd->begin_recording();
        cmd->push_label("Instance retest");
        cmd->record_global_barrier();
        if (together) {
            cmd->record_clear_buffer(arena_buffer, uint64_t(together_word) * sizeof(uint32_t), sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        struct PushC
        {
            uint32_t retest_word;
            uint32_t retest_capacity;
            uint32_t globals_base;
            uint32_t pyramid_word;
            float lod_max_pixels;
            uint32_t tiny_word;
            uint32_t tiny_capacity;
            float tiny_pixels;
        };
        const PushC pc{static_cast<uint32_t>(state.retest.offset() / sizeof(uint32_t)), state.retest_capacity, view_globals_base,
                       state.pyramid_word, kLodMaxErrorPixels, state.tiny_word, state.tiny_capacity, state.tiny_pixels};
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (state.retest_capacity + 63u) / 64u;
        dc.groups_y = 1;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();

        record_cluster_layout(*cmd, state, runs_pipeline.get(), fill_pipeline.get(), arena_buffer, together_word,
                              together_entries_word, true);

        // A plain instanced draw continues its list where the first phase stopped.
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info) || info.cluster_count != 0 || info.second_args_word == kCullNoSlot) {
                continue;
            }
            const uint64_t word = sizeof(uint32_t);
            cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word + 7u) * word, arena_buffer,
                                    uint64_t(info.second_args_word + 1u) * word, sizeof(uint32_t));
            cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * word, arena_buffer,
                                    uint64_t(info.second_args_first_word) * word, sizeof(uint32_t));
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
        rec.held.push_back(std::move(runs_pipeline));
        rec.held.push_back(std::move(fill_pipeline));
    });
}

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_PASS_H
