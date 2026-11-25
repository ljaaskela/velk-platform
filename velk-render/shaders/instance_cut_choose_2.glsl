uint g_count;
uint g_state;
uint g_table;     // the word of the partition's page table in the current one, while writing
bool g_writing;
uint g_at;        // its place in the partition's page table where this thread's next record goes, while writing
bool g_takeover;  // writing records that take over the light of the old ones that were the same
uint g_took;      // how many did
bool g_no_rays;   // the records being made are of a member in a patch for rays (VELK_CUT_RAY_QUAD)
// The partition's quad tree (kQuadNodeWords), or none, its leaves, and this member's leaf.
uint g_tree;
uint g_leaves;
uint g_position;
bool g_leaves_only;  // the leaves are the finest the partition has: its members are not instances
// What a member's roots go to (velk_cut_root): counted, put in the item list from g_root_at, or
// chosen at once by the thread.
#define VELK_ROOTS_COUNT 0u
#define VELK_ROOTS_PUT 1u
#define VELK_ROOTS_DIRECT 2u
uint g_root_mode;
uint g_roots;
uint g_root_at;

// The chunk's work, shared by its threads: items of 4 words, what a record names in its slot
// (its member's slot or plain item, or a quad's tree), an entry word, a node of the entry's tree
// or a record's cluster, and what it is (VELK_CHOOSE_KIND), with VELK_CHOOSE_NO_RAYS for a
// member in a patch for rays. Two lists, which expanding goes between.
#define VELK_CHOOSE_ITEMS 256u
#define VELK_CHOOSE_NODE 0u
#define VELK_CHOOSE_RECORD 1u
#define VELK_CHOOSE_KIND 1u
#define VELK_CHOOSE_NO_RAYS 2u
shared uint s_items[2u * VELK_CHOOSE_ITEMS * 4u];
shared uint s_item_counts[VELK_CHOOSE_ITEMS];
shared uint s_counts[64];
shared uint s_total;
shared uint s_nodes;

uint velk_takeover_hash(uint entry, uint cluster, uint slot)
{
    uint h = (entry * 0x9e3779b1u) ^ (cluster * 0x85ebca77u) ^ (slot * 0xc2b2ae3du);
    h ^= h >> 16u; h *= 0x7feb352du;
    h ^= h >> 15u; h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

// The most slots a lookup in the takeover table tries.
#define VELK_TAKEOVER_PROBES 64u

// The mark of the group cluster `cluster` of the cluster entry `entry` was made from, or with
// `own` of its own group: a cluster made from another's own group is of coarser detail over the
// same place (VELK_LOD_CLUSTER).
uint velk_takeover_mark(uint entry, uint cluster, bool own)
{
    uint word = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS;
    return cull_words.data[word + (own ? VELK_LOD_CLUSTER_OWN_MARK : VELK_LOD_CLUSTER_MARK)];
}

// A takeover table key for the old records made from group `mark`, apart from the records' own.
uint velk_takeover_mark_hash(uint entry, uint mark, uint slot)
{
    return velk_takeover_hash(entry ^ 0x5bd1e995u, mark, slot);
}

// Puts the old record at word `was` in the takeover table under key `h`.
void velk_takeover_insert(uint table, uint mask, uint stamp, uint h, uint was)
{
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * VELK_TAKEOVER_END;
        uint seen = cull_words.data[s + VELK_TAKEOVER_FRAME];
        if (seen == stamp) continue;
        if (atomicCompSwap(cull_words.data[s + VELK_TAKEOVER_FRAME], seen, stamp) == seen) {
            cull_words.data[s + VELK_TAKEOVER_RECORD] = was;
            return;
        }
    }
}

// Lists the old record at word `was` in the takeover table (kCutHeaderWords 30), for the
// records chosen again to find by what they are; a cluster's also by the group it was made from,
// for those of finer detail to find their light (velk_takeover_parent).
void velk_takeover_put(uint cut, uint was)
{
    uint table = cull_words.data[cut + VELK_CUT_TAKEOVER_WORD];
    uint mask = cull_words.data[cut + VELK_CUT_TAKEOVER_SLOTS] - 1u;
    uint stamp = cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME];
    uint entry = cull_words.data[was + VELK_RECORD_ENTRY];
    uint cluster = cull_words.data[was + VELK_RECORD_CLUSTER];
    uint slot = cull_words.data[was + VELK_RECORD_SLOT];
    velk_takeover_insert(table, mask, stamp, velk_takeover_hash(entry, cluster, slot), was);
    if (entry < VELK_CUT_FOR_RAYS) {
        uint mark = velk_takeover_mark(entry, cluster, false);
        if (mark != 0xFFFFFFFFu) velk_takeover_insert(table, mask, stamp, velk_takeover_mark_hash(entry, mark, slot), was);
    }
}

// The old record of coarser detail that cluster `cluster` of `entry` at `slot` replaces: made
// from its group, and of those the one whose own sphere's centre is nearest its own. 0xFFFFFFFF
// for none.
uint velk_takeover_parent(uint cut, uint entry, uint cluster, uint slot)
{
    uint own = velk_takeover_mark(entry, cluster, true);
    if (own == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    uint table = cull_words.data[cut + VELK_CUT_TAKEOVER_WORD];
    uint mask = cull_words.data[cut + VELK_CUT_TAKEOVER_SLOTS] - 1u;
    uint stamp = cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME];
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint sphere = clusters + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
    vec3 at = vec3(uintBitsToFloat(cull_words.data[sphere]), uintBitsToFloat(cull_words.data[sphere + 1u]),
                   uintBitsToFloat(cull_words.data[sphere + 2u]));
    uint h = velk_takeover_mark_hash(entry, own, slot);
    uint best = 0xFFFFFFFFu;
    float nearest = 3.4e38;
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * VELK_TAKEOVER_END;
        if (cull_words.data[s + VELK_TAKEOVER_FRAME] != stamp) break;
        uint was = cull_words.data[s + VELK_TAKEOVER_RECORD];
        if (cull_words.data[was + VELK_RECORD_ENTRY] != entry || cull_words.data[was + VELK_RECORD_SLOT] != slot) continue;
        uint other = cull_words.data[was + VELK_RECORD_CLUSTER];
        if (velk_takeover_mark(entry, other, false) != own) continue;
        uint other_sphere = clusters + other * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
        vec3 d = vec3(uintBitsToFloat(cull_words.data[other_sphere]), uintBitsToFloat(cull_words.data[other_sphere + 1u]),
                      uintBitsToFloat(cull_words.data[other_sphere + 2u])) - at;
        float dd = dot(d, d);
        if (dd < nearest) {
            nearest = dd;
            best = was;
        }
    }
    return best;
}

// The word of the old record that is (entry, cluster, slot), or 0xFFFFFFFF.
uint velk_takeover_find(uint cut, uint entry, uint cluster, uint slot)
{
    uint table = cull_words.data[cut + VELK_CUT_TAKEOVER_WORD];
    uint mask = cull_words.data[cut + VELK_CUT_TAKEOVER_SLOTS] - 1u;
    uint stamp = cull_words.data[cut + VELK_CUT_TAKEOVER_FRAME];
    uint h = velk_takeover_hash(entry, cluster, slot);
    for (uint p = 0u; p < VELK_TAKEOVER_PROBES; ++p) {
        uint s = table + ((h + p) & mask) * VELK_TAKEOVER_END;
        if (cull_words.data[s + VELK_TAKEOVER_FRAME] != stamp) return 0xFFFFFFFFu;
        uint was = cull_words.data[s + VELK_TAKEOVER_RECORD];
        if (cull_words.data[was + VELK_RECORD_ENTRY] == entry && cull_words.data[was + VELK_RECORD_CLUSTER] == cluster
            && cull_words.data[was + VELK_RECORD_SLOT] == slot) {
            return was;
        }
    }
    return 0xFFFFFFFFu;
}

// Counts a record, or writes it at this thread's next place, with the light of the old record
// that was the same.
void velk_cut_record(uint entry, uint cluster, uint slot)
{
    if (g_writing) {
        uint at = g_at++;
        // Never past the pages the count gave the partition.
        if (at >= cull_words.data[g_state + VELK_PART_COUNT]) return;
        uint place = velk_cut_place(g_table, at);
        uint record = velk_cut_record_word(pc.cut_word, place);
        cull_words.data[record + VELK_RECORD_ENTRY] = entry;
        cull_words.data[record + VELK_RECORD_CLUSTER] = cluster;
        cull_words.data[record + VELK_RECORD_SLOT] = slot;
        // Whether rays leave it out, for the tree pass.
        uint mark = cull_words.data[pc.cut_word + VELK_CUT_RAY_MARKS_WORD] + (place >> 5u);
        uint bit = 1u << (place & 31u);
        if (g_no_rays) atomicOr(cull_words.data[mark], bit);
        else atomicAnd(cull_words.data[mark], ~bit);
        uint light = 0xFFFFFFFFu;
        uint radiance = 0xFFFFFFFFu;
        uint radiance_dir = 0u;
        if (g_takeover) {
            uint was = velk_takeover_find(pc.cut_word, entry, cluster, slot);
            if (was != 0xFFFFFFFFu) {
                light = cull_words.data[was + VELK_RECORD_LIGHT];
                radiance = cull_words.data[was + VELK_RECORD_RADIANCE];
                radiance_dir = cull_words.data[was + VELK_RECORD_RADIANCE_DIR];
                ++g_took;
            } else if (entry < VELK_CUT_FOR_RAYS) {
                // Finer detail where coarser was: its light starts from the coarser record's, a
                // traced one value as it is, points by their page (kLightInherit), and its indirect
                // light likewise.
                uint parent = velk_takeover_parent(pc.cut_word, entry, cluster, slot);
                if (parent != 0xFFFFFFFFu) {
                    // kLightOneValue with kLightOneTraced, under kLightPending; or a page, under
                    // kLightInherit.
                    uint had = cull_words.data[parent + VELK_RECORD_LIGHT];
                    if (had >= 0x80000000u && had < 0xFFFFFFFEu && (had & 0x40000000u) != 0u) {
                        light = had;
                    } else if (had < 0x40000000u) {
                        light = 0x40000000u | had;
                    }
                    radiance = cull_words.data[parent + VELK_RECORD_RADIANCE];
                    radiance_dir = cull_words.data[parent + VELK_RECORD_RADIANCE_DIR];
                }
            }
        }
        cull_words.data[record + VELK_RECORD_LIGHT] = light;
        cull_words.data[record + VELK_RECORD_RADIANCE] = radiance;
        cull_words.data[record + VELK_RECORD_RADIANCE_DIR] = radiance_dir;
    }
    ++g_count;
}

void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    // Traced: written, or left out past the count.
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g_writing && g.trace != VELK_TRACE_NONE) {
        uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
        uint own = cluster + VELK_LOD_CLUSTER_OWN_SPHERE;
        vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[own]), uintBitsToFloat(cull_words.data[own + 1u]),
                                         uintBitsToFloat(cull_words.data[own + 2u]))
                    + (world[3].xyz - g.cam_pos.xyz);
        float radius = uintBitsToFloat(cull_words.data[own + 3u]) * scale;
        if (velk_trace_sphere(g, center, radius)) {
            velk_trace_note(g, VELK_TRACE_CHOOSE, g_at < cull_words.data[g_state + VELK_PART_COUNT] ? 0u : 1u, entry, c, slot);
        }
    }
    velk_cut_record(entry, c, slot);
}

// A cluster at the light's level over drawn ones finer than it: a record for rays only.
void velk_lod_light_chosen(uint entry, uint c, uint slot)
{
    velk_cut_record(entry | VELK_CUT_FOR_RAYS, c, slot);
}

// The trace of the walk (kTraceWalk): what the walk of entry g_walk_entry, for the partition
// at g_state, made of cluster `c` (its words at `clusters`), when the traced pixel's ray passes
// its own sphere.
#define VELK_TRACE_WALK
uint g_walk_entry;
void velk_trace_walk(uint clusters, uint c, mat4 world, float scale, uint what)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE) return;
    // The entry traced in every pass, its items' expansion too; else only while writing.
    bool named = cull_words.data[g.trace + 4u] == g_walk_entry;
    if (!g_writing && !named) return;
    uint own = clusters + c * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_OWN_SPHERE;
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[own]), uintBitsToFloat(cull_words.data[own + 1u]),
                                     uintBitsToFloat(cull_words.data[own + 2u]))
                + (world[3].xyz - g.cam_pos.xyz);
    // On the ray, or of the entry traced wherever it is.
    if (cull_words.data[g.trace + 4u] != g_walk_entry
        && !velk_trace_sphere(g, center, uintBitsToFloat(cull_words.data[own + 3u]) * scale)) return;
    velk_trace_note(g, 7u, what, g_walk_entry, c, g_state);
}

// The numbers a decision of the walk of the entry traced went by (kTraceNumbers): `what` (a
// node's reference or a cluster, and which number), the number as stored and what it is on
// screen from the partition's viewpoint, in pixels.
void velk_trace_numbers(uint what, float stored, float pixels)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE || cull_words.data[g.trace + 4u] != g_walk_entry) return;
    velk_trace_note(g, 10u, 0u, what, floatBitsToUint(stored), floatBitsToUint(pixels));
}

// The child `ref` of the node whose child words start at `child`, in the tree of the entry
// traced: its node's index and which child it is.
void velk_trace_shape(uint ref, uint child)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE || cull_words.data[g.trace + 4u] != g_walk_entry) return;
    uint offset = child - cull_words.data[g_walk_entry + VELK_CLUSTER_ENTRY_NODES];
    velk_trace_note(g, 11u, 0u, ref, offset / VELK_LOD_NODE_STRIDE, (offset % VELK_LOD_NODE_STRIDE) / 8u);
}

// Likewise a node of the tree, its sphere at `at` (world space) of `reach`, skipped with all
// under it (`what`), for which `node` is its reference.
void velk_trace_node(vec3 at, float reach, uint node, uint what)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    if (g.trace == VELK_TRACE_NONE) return;
    bool named = cull_words.data[g.trace + 4u] == g_walk_entry;
    if (!g_writing && !named) return;
    if (!named && !velk_trace_sphere(g, at - g.cam_pos.xyz, reach)) return;
    velk_trace_note(g, 7u, what, g_walk_entry, node | 0x40000000u, g_state | (g_writing ? 0u : 0x80000000u));
}
