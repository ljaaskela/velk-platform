#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint cut_word;
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float lod_max_pixels;
    uint write;
    uint items_word;
    uint objects_word;
    uint next_word;
    uint head_word;
    uint object_slots;
    uint partition_table;  // the cut's partition table, whose entries name their quad trees
    float quad_pixels;     // kQuadPixels
    float light_pixels;    // the light's level: lod_max_pixels times kLightPixels over kLodElementPixels
    uint descriptors_word; // the instanced visuals' item descriptors (kItemDescriptorHeaderWords)
    uint descriptor_count;
} pc;

#define VELK_LOD_LIGHT
#velk_include VELK_CUT_GLSL
#velk_include VELK_TRACE_GLSL
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
#velk_include VELK_LOD_SELECT_GLSL
// The instance a record's `named` is of: its item's.
uint velk_cut_named_instance(uint named)
{
    return cull_words.data[pc.items_word + named * VELK_PLAIN_ITEM_WORDS];
}

float velk_cut_scale(mat4 world)
{
    return sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
}

// Walks the tree of `entry` from `node` down for `named`, choosing what it holds.
void velk_cut_walk(uint named, uint entry, uint node, vec3 viewpoint)
{
    CullInstance ci = cull_instances.data[velk_cut_named_instance(named)];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    GlobalData globals = velk_globals.data[pc.globals_base];
    g_walk_entry = entry;
    velk_lod_walk(entry, node, named, world, velk_cut_scale(world), viewpoint, viewpoint, globals.view_projection,
                  globals.viewport.y, pc.lod_max_pixels);
}

// A root of a member's choice: a record, or the root of one of its trees. Counted, put in the
// item list, or chosen at once, as g_root_mode says.
void velk_cut_root(uint kind, uint named, uint entry, uint ref, vec3 viewpoint)
{
    if (g_root_mode == VELK_ROOTS_COUNT) {
        ++g_roots;
    } else if (g_root_mode == VELK_ROOTS_PUT) {
        uint at = g_root_at++;
        if (at >= VELK_CHOOSE_ITEMS) return;
        s_items[at * 4u] = named;
        s_items[at * 4u + 1u] = entry;
        s_items[at * 4u + 2u] = ref;
        s_items[at * 4u + 3u] = kind | (g_no_rays ? VELK_CHOOSE_NO_RAYS : 0u);
    } else if (kind == VELK_CHOOSE_RECORD) {
        // Traced: a root chosen as it is, of the entry traced.
        GlobalData g = velk_globals.data[pc.globals_base];
        if (g_writing && g.trace != VELK_TRACE_NONE && cull_words.data[g.trace + 4u] == entry) {
            velk_trace_note(g, 7u, 11u, entry, ref, g_state);
        }
        velk_cut_record(entry, ref, named);
    } else {
        velk_cut_walk(named, entry, ref, viewpoint);
    }
}

// The items a node item at word `w` of s_items becomes: each of its children that is not
// skipped, in order, a node to walk or a record. Counted, or with `put` written from item `to`.
uint velk_cut_expand(uint w, vec3 viewpoint, bool put, uint to)
{
    uint named = s_items[w];
    uint entry = s_items[w + 1u];
    uint node = s_items[w + 2u];
    g_walk_entry = entry & ~VELK_CUT_FOR_RAYS;
    CullInstance ci = cull_instances.data[velk_cut_named_instance(named)];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = velk_cut_scale(world);
    GlobalData globals = velk_globals.data[pc.globals_base];
    mat4 vp = globals.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint at = cull_words.data[entry + VELK_CLUSTER_ENTRY_NODES] + node * VELK_LOD_NODE_STRIDE;
    uint no_rays = s_items[w + 3u] & VELK_CHOOSE_NO_RAYS;
    if (!put) ++g_visited;
    uint n = 0u;
    for (uint k = 0u; k < 2u; ++k) {
        uint c;
        uint what = velk_lod_child(at + k * 8u, clusters, world, scale, viewpoint, vp, pixels_per_unit, pc.lod_max_pixels, c);
        if (what == VELK_LOD_SKIP) continue;
        if (put) {
            uint o = (to + n) * 4u;
            s_items[o] = named;
            s_items[o + 1u] = what == VELK_LOD_FOR_RAYS ? entry | VELK_CUT_FOR_RAYS : entry;
            s_items[o + 2u] = c;
            s_items[o + 3u] = (what == VELK_LOD_DESCEND ? VELK_CHOOSE_NODE : VELK_CHOOSE_RECORD) | no_rays;
        }
        ++n;
    }
    return n;
}

// The roots of an instance's choice; `named` is what its records name: its item.
void velk_cut_choose(uint named, vec3 viewpoint, uint table)
{
    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    if (entries == 0u) {
        velk_cut_root(VELK_CHOOSE_RECORD, named, 0xFFFFFFFFu, table, viewpoint);
        return;
    }
    for (uint e = 0u; e < entries; ++e) {
        velk_cut_root(VELK_CHOOSE_NODE, named, first_entry + e * VELK_CLUSTER_ENTRY_WORDS, 0u, viewpoint);
    }
}

// Chooses for one instance; `named` is what its records name: its item. Nothing for an instance
// without a place in its raster region.
void velk_cut_instance(uint named, uint instance, uint table_word, vec3 viewpoint)
{
    if (instance == 0xFFFFFFFFu) return;
    mat4 world = cull_instances.data[instance].world_matrix;
    if (world[3][3] == 0.0) return;
    // An item for rays only (no table): the whole object.
    if (table_word == 0xFFFFFFFFu) {
        velk_cut_root(VELK_CHOOSE_RECORD, named, 0xFFFFFFFFu, 0xFFFFFFFFu, viewpoint);
        return;
    }
    velk_cut_choose(named, viewpoint, table_word + cull_instances.data[instance].params.y * VELK_TABLE_END);
}

// The topmost node of the quad tree over this member that is small enough on screen from
// `viewpoint`, or none when its own quad is not. A node's size is its quad's longer side, seen
// at the nearest point of its sphere. A parent's quad keeps its children's area with the larger
// child's aspect and its sphere holds theirs, so sizes only grow going up, and the first too
// large ends it.
uint velk_quad_over(vec3 viewpoint, float pixels_per_unit)
{
    uint node = g_leaves - 1u + g_position;
    uint chosen = 0xFFFFFFFFu;
    for (uint step = 0u; step < 32u; ++step) {
        uint w = g_tree + node * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 2u]));
        float side = 2.0 * max(length(u), length(v));
        float near = length(center - viewpoint) - radius;
        // Out of view, no finer than the light's level.
        bool out_of_view = velk_lod_out_of_view(center, radius, velk_globals.data[pc.globals_base].view_projection);
        g_out_of_view = g_out_of_view || out_of_view;
        float limit = out_of_view ? max(pc.quad_pixels, pc.light_pixels) : pc.quad_pixels;
        if (near <= 0.0 || side * pixels_per_unit > limit * near) break;
        chosen = node;
        if (node == 0u) break;
        node = (node - 1u) / 2u;
    }
    return chosen;
}

// The patch for rays over this member: the topmost node of the quad tree over it no wider on
// screen than the light spacing from `viewpoint`, measured as velk_quad_over measures, or none
// when its own quad is wider.
uint velk_quad_patch(vec3 viewpoint, float pixels_per_unit)
{
    float spacing = pc.light_pixels * pc.quad_pixels / pc.lod_max_pixels;
    uint node = g_leaves - 1u + g_position;
    uint chosen = 0xFFFFFFFFu;
    for (uint step = 0u; step < 32u; ++step) {
        uint w = g_tree + node * VELK_QUAD_NODE_WORDS;
        float radius = uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_RADIUS]);
        if (radius < 0.0) break;
        vec3 center = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 1u]),
                           uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_CENTER + 2u]));
        vec3 u = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_U + 2u]));
        vec3 v = vec3(uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V]), uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 1u]),
                      uintBitsToFloat(cull_words.data[w + VELK_QUAD_NODE_V + 2u]));
        float near = length(center - viewpoint) - radius;
        if (near <= 0.0 || 2.0 * max(length(u), length(v)) * pixels_per_unit > spacing * near) break;
        chosen = node;
        if (node == 0u) break;
        node = (node - 1u) / 2u;
    }
    return chosen;
}

// The leaf of the first member under `node`.
uint velk_quad_first(uint node)
{
    while (node < g_leaves - 1u) node = node * 2u + 1u;
    return node - (g_leaves - 1u);
}


// Chooses for a member of a chunk: an instance's slot, or an object.
void velk_cut_member(uint member, vec3 viewpoint)
{
    g_no_rays = false;
    // Under a quad small enough: the first member under it records the quad, the rest nothing.
    if (g_tree != 0xFFFFFFFFu) {
        GlobalData globals = velk_globals.data[pc.globals_base];
        mat4 vp = globals.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
        // In a patch for rays: the first member under it records the patch, and rays leave out
        // the records of every member under it.
        uint patch_node = velk_quad_patch(viewpoint, pixels_per_unit);
        if (patch_node != 0xFFFFFFFFu) {
            if (velk_quad_first(patch_node) == g_position) {
                velk_cut_root(VELK_CHOOSE_RECORD, g_tree, VELK_CUT_RAY_QUAD, g_tree + patch_node * VELK_QUAD_NODE_WORDS, viewpoint);
            }
            g_no_rays = true;
        }
        uint quad = velk_quad_over(viewpoint, pixels_per_unit);
        // Where nothing is small enough, a partition without its instances still has its leaf.
        if (quad == 0xFFFFFFFFu && g_leaves_only) quad = g_leaves - 1u + g_position;
        if (quad != 0xFFFFFFFFu) {
            if (velk_quad_first(quad) == g_position
                && uintBitsToFloat(cull_words.data[g_tree + quad * VELK_QUAD_NODE_WORDS + VELK_QUAD_NODE_RADIUS]) >= 0.0) {
                velk_cut_root(VELK_CHOOSE_RECORD, g_tree, VELK_CUT_QUAD, g_tree + quad * VELK_QUAD_NODE_WORDS, viewpoint);
            }
            return;
        }
    }
    if (g_leaves_only) return;
    // An instanced visual's object: its one item, by its id, through its visual's descriptor.
    uint at = pc.descriptors_word;
    for (uint k = 0u; k < pc.descriptor_count && k < 4096u; ++k) {
        uint base = cull_words.data[at + VELK_ITEM_DESCRIPTOR_OBJECT_BASE];
        if (base != 0xFFFFFFFFu && member >= base && member - base < cull_words.data[at + VELK_ITEM_DESCRIPTOR_SLOTS]) {
            uint item = cull_words.data[at + VELK_ITEM_DESCRIPTOR_FIRST] + member - base;
            uint w = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[w + VELK_PLAIN_ITEM_INSTANCE], cull_words.data[w + VELK_PLAIN_ITEM_TABLE], viewpoint);
            return;
        }
        at += VELK_ITEM_DESCRIPTOR_HEADER_WORDS + min(cull_words.data[at + VELK_ITEM_DESCRIPTOR_MESHES], 4096u) * VELK_ITEM_MESH_WORDS;
    }
    // A plain object: each of its items. An object has a few at most; the bound only keeps a
    // broken chain from running on.
    uint object_slot = member & 0x3FFFFFFu;
    uint item = object_slot < pc.object_slots ? cull_words.data[pc.head_word + object_slot] : 0xFFFFFFFFu;
    for (uint n = 0u; n < 64u && item != 0xFFFFFFFFu; ++n) {
        if (cull_words.data[pc.objects_word + item] == member) {
            uint at = pc.items_word + item * VELK_PLAIN_ITEM_WORDS;
            velk_cut_instance(item, cull_words.data[at + VELK_PLAIN_ITEM_INSTANCE], cull_words.data[at + VELK_PLAIN_ITEM_TABLE], viewpoint);
        }
        item = cull_words.data[pc.next_word + item];
    }
}

void main()
{
    uint cut = pc.cut_word;
    uint chunk_index = gl_WorkGroupID.x;
    uint chunk = cull_words.data[cut + VELK_CUT_CHOOSE_WORD] + chunk_index * VELK_CHUNK_END;
    uint lane = gl_LocalInvocationIndex;
    uint member = cull_words.data[cull_words.data[chunk + VELK_CHUNK_FIRST_SLOT] + lane];
    uint state = velk_cut_state(cut, cull_words.data[chunk + VELK_CHUNK_PARTITION]);
    uint chunk_word = cull_words.data[cut + VELK_CUT_CHUNK_COUNTS_WORD] + chunk_index;
    // A work group is one chunk of one partition, so this is the same for all its threads. A
    // partition that found no room chooses on the next frame.
    bool choosing = pc.write == 0u || cull_words.data[state + VELK_PART_CHOOSING] != 0u;
    vec3 viewpoint = vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                          uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));

    g_count = 0u;
    g_state = state;
    g_table = velk_cut_table(cut, true) + cull_words.data[state + VELK_PART_TABLE_FIRST];
    g_writing = false;
    g_no_rays = false;
    g_out_of_view = false;
    g_visited = 0u;
    g_view_cam = velk_globals.data[pc.globals_base].cam_pos.xyz;
    // Where this member is in its partition's list, which its quad tree's leaves follow.
    g_tree = 0xFFFFFFFFu;
    g_leaves = 0u;
    g_position = 0u;
    g_leaves_only = false;
    if (pc.partition_table != 0xFFFFFFFFu) {
        uint entry = pc.partition_table + cull_words.data[chunk + VELK_CHUNK_PARTITION] * VELK_CULL_ENTRY_WORDS;
        uint leaves = cull_words.data[entry + VELK_PARTITION_ENTRY_TREE_LEAVES] & ~VELK_QUAD_LEAVES_ONLY;
        uint position = cull_words.data[chunk + VELK_CHUNK_FIRST_SLOT] - cull_words.data[entry + VELK_PARTITION_ENTRY_FIRST_CHUNK] + lane;
        if (cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE] != 0xFFFFFFFFu && position < leaves) {
            g_tree = cull_words.data[entry + VELK_PARTITION_ENTRY_QUAD_TREE];
            g_leaves = leaves;
            g_position = position;
            g_leaves_only = (cull_words.data[entry + VELK_PARTITION_ENTRY_TREE_LEAVES] & VELK_QUAD_LEAVES_ONLY) != 0u;
        }
    }
    // The same for all its threads: a work group is one chunk of one partition.
    if (!choosing) return;

    // The members' roots, in member order: counted, then put in the item list. A member whose
    // roots do not fit after the ones before it chooses alone, after the items.
    g_root_mode = VELK_ROOTS_COUNT;
    g_roots = 0u;
    if (member != 0xFFFFFFFFu) velk_cut_member(member, viewpoint);
    s_counts[lane] = g_roots;
    barrier();
    uint before = 0u;
    for (uint i = 0u; i < lane; ++i) before += s_counts[i];
    bool alone = g_roots != 0u && before + g_roots > VELK_CHOOSE_ITEMS;
    uint items = 0u;
    for (uint i = 0u; i < 64u && items + s_counts[i] <= VELK_CHOOSE_ITEMS; ++i) items += s_counts[i];
    if (g_roots != 0u && !alone) {
        g_root_mode = VELK_ROOTS_PUT;
        g_root_at = before;
        velk_cut_member(member, viewpoint);
    }
    barrier();

    // Expanded together, a level at a time, each node item into its children in order, until
    // there are enough nodes for each thread to walk a few small trees: the longest walk is then
    // one of those, not a whole large mesh's tree. Both lists stay in member order.
    uint from_list = 0u;
    for (uint round = 0u; round < 16u; ++round) {
        if (lane == 0u) s_nodes = 0u;
        barrier();
        for (uint i = lane; i < items; i += 64u) {
            if ((s_items[(from_list * VELK_CHOOSE_ITEMS + i) * 4u + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE) atomicAdd(s_nodes, 1u);
        }
        barrier();
        uint nodes = s_nodes;
        barrier();
        if (nodes == 0u || nodes >= 128u || items * 2u > VELK_CHOOSE_ITEMS) break;
        uint to_list = 1u - from_list;
        for (uint i = lane; i < items; i += 64u) {
            uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
            s_item_counts[i] = (s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE ? velk_cut_expand(w, viewpoint, false, 0u) : 1u;
        }
        barrier();
        if (lane == 0u) {
            uint run = 0u;
            for (uint i = 0u; i < items; ++i) {
                uint n = s_item_counts[i];
                s_item_counts[i] = run;
                run += n;
            }
            s_total = run;
        }
        barrier();
        for (uint i = lane; i < items; i += 64u) {
            uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
            uint to = to_list * VELK_CHOOSE_ITEMS + s_item_counts[i];
            if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_NODE) {
                velk_cut_expand(w, viewpoint, true, to);
            } else {
                for (uint k = 0u; k < 4u; ++k) s_items[to * 4u + k] = s_items[w + k];
            }
        }
        items = s_total;
        from_list = to_list;
        barrier();
    }

    // Each item's records, counted: a record is one, a node's tree is walked.
    uint most = 0u;
    for (uint i = lane; i < items; i += 64u) {
        uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
        g_count = 0u;
        g_no_rays = (s_items[w + 3u] & VELK_CHOOSE_NO_RAYS) != 0u;
        if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_RECORD) {
            velk_cut_record(s_items[w + 1u], s_items[w + 2u], s_items[w]);
        } else {
            velk_cut_walk(s_items[w], s_items[w + 1u], s_items[w + 2u], viewpoint);
        }
        s_item_counts[i] = g_count;
        most = max(most, g_count);
    }
    uint alone_count = 0u;
    if (alone) {
        g_root_mode = VELK_ROOTS_DIRECT;
        g_count = 0u;
        velk_cut_member(member, viewpoint);
        alone_count = g_count;
        most = max(most, g_count);
    }
    barrier();
    // Where each item's records start, in order, then those of the members alone.
    if (lane == 0u) {
        uint run = 0u;
        for (uint i = 0u; i < items; ++i) {
            uint n = s_item_counts[i];
            s_item_counts[i] = run;
            run += n;
        }
        s_total = run;
    }
    s_counts[lane] = alone_count;
    barrier();
    uint alone_at = s_total;
    for (uint i = 0u; i < lane; ++i) alone_at += s_counts[i];
    uint total = s_total;
    for (uint i = 0u; i < 64u; ++i) total += s_counts[i];
    // Some of it chosen coarse for being out of view: the partition chooses again on a turn.
    if (g_out_of_view) cull_words.data[state + VELK_PART_OUT_OF_VIEW] = 1u;

    if (pc.write == 0u) {
        // The longest walk and the most records of one walk, for the log.
        if (g_visited != 0u) atomicMax(cull_words.data[cut + VELK_CUT_LONGEST_WALK], g_visited);
        if (most != 0u) atomicMax(cull_words.data[cut + VELK_CUT_MOST_CHOSEN], most);
        if (lane == 0u) {
            cull_words.data[chunk_word] = total;
            if (total != 0u) atomicAdd(cull_words.data[state + VELK_PART_COUNT], total);
        }
        // This chunk's share of the partition's old records goes in the takeover table, for its
        // records written next to find: on its old pages, by its old page table in the last one
        // (VELK_PART_LAST_TABLE_FIRST, OLD_COUNT).
        uint old = cull_words.data[state + VELK_PART_OLD_COUNT];
        if (old != 0u) {
            uint chunks = cull_words.data[state + VELK_PART_CHUNKS];
            uint per = (old + chunks - 1u) / chunks;
            uint from = (chunk_index - cull_words.data[state + VELK_PART_CHUNK_START]) * per;
            uint to = min(from + per, old);
            uint old_table = velk_cut_table(cut, false) + cull_words.data[state + VELK_PART_LAST_TABLE_FIRST];
            for (uint i = from + lane; i < to; i += 64u) velk_takeover_put(cut, velk_cut_record_word(cut, velk_cut_place(old_table, i)));
        }
        return;
    }

    // Written: each item from its place in the chunk's range, then the members alone. Each record
    // takes over the light of the old record that was the same.
    uint first = cull_words.data[chunk_word];
    g_writing = true;
    g_takeover = cull_words.data[state + VELK_PART_OLD_COUNT] != 0u;
    g_took = 0u;
    g_count = 0u;
    for (uint i = lane; i < items; i += 64u) {
        uint w = (from_list * VELK_CHOOSE_ITEMS + i) * 4u;
        g_at = first + s_item_counts[i];
        g_no_rays = (s_items[w + 3u] & VELK_CHOOSE_NO_RAYS) != 0u;
        if ((s_items[w + 3u] & VELK_CHOOSE_KIND) == VELK_CHOOSE_RECORD) {
            velk_cut_record(s_items[w + 1u], s_items[w + 2u], s_items[w]);
        } else {
            velk_cut_walk(s_items[w], s_items[w + 1u], s_items[w + 2u], viewpoint);
        }
    }
    if (alone) {
        g_root_mode = VELK_ROOTS_DIRECT;
        g_at = first + alone_at;
        velk_cut_member(member, viewpoint);
    }
    if (g_count != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_REWRITTEN], g_count);
    if (g_took != 0u) atomicAdd(cull_words.data[cut + VELK_CUT_TOOK_OVER], g_took);
}
