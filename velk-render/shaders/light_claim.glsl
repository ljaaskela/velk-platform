// Puts `record`, now pending, on the search list with the light it had and its instance; with
// no room, gives its light back.
void velk_light_search(uint record, uint was, uint instance)
{
    uint at = atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SEARCHED], 1u);
    if (at >= VELK_LIGHT_SEARCH_CAPACITY) {
        light_words.data[record + VELK_RECORD_LIGHT] = was;
        return;
    }
    uint list = light_words.data[pc.pool_word + VELK_POOL_SEARCH_WORD] + at * VELK_SEARCH_ENTRY_WORDS;
    light_words.data[list + VELK_SEARCH_ENTRY_RECORD] = record;
    light_words.data[list + VELK_SEARCH_ENTRY_WAS] = was;
    light_words.data[list + VELK_SEARCH_ENTRY_INSTANCE] = instance;
}

// Puts a first trace of `rays` rays on the trace list (kLightTraceWords); nothing when the list or
// the frame's budget of rays is full, and it is listed again on a later frame.
void velk_light_trace(uint kind, uint at, uint instance, uint table, uint rays)
{
    velk_light_list_entry(pc.pool_word, kind, at, instance, table, rays);
}

// Lists the coarse light of the own points whose first page is `page`, when it is due.
void velk_light_list_record(uint page, uint instance, uint frame)
{
    if (page >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
    uint first = velk_light_page(pc.pool_word, page);
    light_words.data[first + VELK_PAGE_READ] = 1u;
    velk_light_list_coarse(pc.pool_word, page, instance, frame, pc.lights);
}

// Puts an ask for pieces on the split list: for the children of the piece at page `page`, or with
// VELK_LIGHT_SPLIT_ROOT in `how`, for the root of the triangle in its low 8 bits of the record
// whose first page is `page`, its grid's level in bits 16 to 18; down to the depth in its bits 8
// to 15. False when the list is full.
bool velk_light_split(uint page, uint how)
{
    uint at = atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SPLITS], 1u);
    if (at >= VELK_LIGHT_SPLIT_CAPACITY) {
        atomicAdd(light_words.data[pc.pool_word + VELK_POOL_SUM_SPLITS_REFUSED], 1u);
        return false;
    }
    uint w = light_words.data[pc.pool_word + VELK_POOL_SPLIT_WORD] + at * VELK_SPLIT_ENTRY_WORDS;
    light_words.data[w + VELK_SPLIT_ENTRY_PAGE] = page;
    light_words.data[w + VELK_SPLIT_ENTRY_HOW] = how;
    return true;
}

// The deepest depth from `from` on whose pieces' points are at least VELK_LIGHT_COARSE_PIECES
// times `light_pixels` apart, in a root `tri_px` pixels across whose pieces' grids are of level
// `grid`; `from` itself when none deeper is.
uint velk_piece_coarse_depth(float tri_px, uint grid, float light_pixels, uint from)
{
    float over = tri_px / (float(grid) * VELK_LIGHT_COARSE_PIECES * light_pixels);
    uint deepest = over >= 2.0 ? uint(min(floor(log2(over)), float(VELK_PIECE_DEEPEST))) : 0u;
    return max(from, deepest);
}

// The piece starting at word `at` read in the pool's frame `now`, once a frame.
void velk_light_piece_read(uint at, uint now)
{
    uint read = light_words.data[at + VELK_PIECE_READ];
    if ((read & VELK_PIECE_FRAME_BITS) != now) {
        atomicCompSwap(light_words.data[at + VELK_PIECE_READ], read, now | (read & VELK_PIECE_SAME_BITS));
    }
}

// Lists the points of light the pixel at weights `b` of triangle `triangle` (corners `corners`)
// of the record whose first page starts at `first` reads on its way down (velk_piece_way), when
// they are due: its coarse level's three around it as coarse, then those of the level it
// reaches, which is its coarse level where that level's cell is exact or not known yet
// (velk_way_cell); and the triangle's vertex points as coarse whatever the coarse level is, which rays
// that hit the triangle read. A piece it reaches is read this frame. Where the level it reaches
// is farther apart than the light spacing and has nothing under it, it asks for what comes next,
// the triangle's root under its vertex points, or a piece's children: always while the next
// level's points would still be VELK_LIGHT_COARSE_PIECES times the spacing apart, down to the
// deepest such level at once; below that, only where its points around the pixel are traced and
// differ, or are not all fully lit or fully blocked for every light: where an edge or a penumbra
// passes. Once a frame a piece; a root, once until it is made. `tri_px` is the triangle's
// longest edge on screen where the pixel is. Its root is `r` (velk_light_root_of), its triangle's,
// `root_px` pixels across.
void velk_light_list_way(uint first, uvec3 corners, VelkLightRoot r, vec3 b, float tri_px, float root_px, uint instance,
                         uint frame)
{
    uint pool = pc.pool_word;
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    uint root_word = velk_light_root_word(pool, first, r.slot);
    uint root = light_words.data[root_word];
    VelkPieceWay way = velk_piece_way(pool, root, b, tri_px, r, root_px, pc.light_pixels);
    // Where its coarse cell is exact, or not known yet, it goes no further down: judged by its
    // points' light even when a change made them stale, so that the finer points an edge needs
    // are traced while it moves; the lighting judges it again with what is traced.
    uint cell = velk_way_cell(pool, first, corners, way, false);
    if (cell < 2u) velk_way_stop_at_coarse(way);
    bool on_piece = way.piece != VELK_PIECE_VERTICES;
    uint piece_first = on_piece ? velk_light_page(pool, way.piece) : 0u;
    uint now = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    if (on_piece) velk_light_piece_read(piece_first, now);

    // The vertex points: its coarse level when it is them, else read only by bounces.
    for (uint k = 0u; k < 3u; ++k) {
        velk_light_list_point(pool, first, velk_way_vertex(first, corners[k]), instance, frame,
                              way.coarse == VELK_PIECE_VERTICES);
    }
    uint words[3];
    vec3 w;
    if (way.coarse != VELK_PIECE_VERTICES) {
        velk_piece_around(pool, way.coarse, way.coarse_at, words, w);
        uint coarse_first = velk_light_page(pool, way.coarse);
        for (uint k = 0u; k < 3u; ++k) {
            velk_light_list_piece_point(pool, way.coarse, (words[k] - coarse_first) / 2u, first, instance, frame, true);
        }
    }
    // The level it reaches: listed when it is not the coarse level, whose points it then holds.
    velk_way_around(pool, first, corners, way.piece, way.at, words, w);
    if (way.piece != way.coarse) {
        for (uint k = 0u; k < 3u; ++k) {
            if (w[k] > 0.0) velk_light_list_piece_point(pool, way.piece, (words[k] - piece_first) / 2u, first, instance, frame, false);
        }
    }

    if (pc.again != 0u) return;
    if (way.spacing <= pc.light_pixels) return;
    // Nothing under it yet: a piece without children, or the vertex points without a root.
    if (on_piece && (way.depth >= VELK_PIECE_DEEPEST || light_words.data[piece_first + VELK_PIECE_CHILDREN] < pages)) return;
    if (!on_piece && (root != VELK_LIGHT_NONE || tri_px <= 2.0 * pc.light_pixels)) return;
    uint level = on_piece ? way.root_level : velk_piece_root_level(tri_px, pc.light_pixels);
    uint next = on_piece ? way.depth + 1u : 0u;
    bool edge = velk_piece_spacing(next, level, root_px) < VELK_LIGHT_COARSE_PIECES * pc.light_pixels;
    uint depth = next;
    if (!edge) {
        uint last = on_piece ? way.depth + VELK_PIECE_LEVELS_AT_ONCE : VELK_PIECE_LEVELS_AT_ONCE;
        depth = min(velk_piece_coarse_depth(root_px, VELK_PIECE_LEVEL, pc.light_pixels, next), min(last, VELK_PIECE_DEEPEST));
    } else {
        uint s0 = light_words.data[words[0]];
        uint s1 = light_words.data[words[1]];
        uint s2 = light_words.data[words[2]];
        if (s0 == VELK_LIGHT_NONE || s1 == VELK_LIGHT_NONE || s2 == VELK_LIGHT_NONE) return;
        // Every light's four bits all clear or all set, the same at all three.
        if (s0 == s1 && s1 == s2 && s0 == (s0 & 0x11111111u) * 15u) return;
    }
    // The pixel's way down the levels made at once: which child it is in at each, 2 bits a level
    // (VELK_SPLIT_PATH_SHIFT), so that only the pieces along it are split further.
    vec3 l = on_piece ? way.at : r.at;
    uint path = 0u;
    for (uint k = 0u; k + 1u < VELK_PIECE_LEVELS_AT_ONCE; ++k) path |= velk_piece_step(l) << (2u * k);
    path <<= VELK_SPLIT_PATH_SHIFT;
    if (edge) path |= VELK_LIGHT_SPLIT_EDGE;
    if (on_piece) {
        uint before = atomicOr(light_words.data[piece_first + VELK_PIECE_READ], VELK_PIECE_ASKED);
        if ((before & VELK_PIECE_ASKED) == 0u) velk_light_split(way.piece, (depth << 8u) | path);
    } else if (atomicCompSwap(light_words.data[root_word], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) == VELK_LIGHT_NONE) {
        if (!velk_light_split(light_words.data[first + VELK_PAGE_FIRST], VELK_LIGHT_SPLIT_ROOT | r.slot | (depth << 8u) | (level << 16u) | path)) {
            light_words.data[root_word] = VELK_LIGHT_NONE;
        }
    }
}


// Lists the points of light the pixel with ids `ids`, of draw `d`, reads from the record whose
// first page starts at `first`, those it reads with any weight, when they are due
// (velk_light_list_point). Where the pixel is in its triangle, and its triangle's size, are as
// its lighting finds them too (velk_claim_weights, velk_claim_tri_px), so that both read the same
// points.
void velk_light_list_pixel(uint first, VelkDrawData d, uvec2 ids, uint instance, uint frame, GlobalData g)
{
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint triangle = velk_ids_triangle(d, ids);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    uvec3 corners = uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
    vec3 b = velk_claim_weights();
    // In pieces: the pixel's way down its triangle's levels, from its vertex points.
    uint pool = pc.pool_word;
    VelkLightRoot r = velk_light_root_of(first, triangle, b, velk_claim_tri_size());
    if (r.slot != VELK_LIGHT_NONE) {
        float px_per_world = velk_claim_px_per_world(g);
        float tri_px = velk_claim_tri_size() * px_per_world;
        g_listed_by = tri_px < 8.0 ? VELK_LIGHT_LISTED_SMALL : tri_px < 64.0 ? VELK_LIGHT_LISTED_MID : VELK_LIGHT_LISTED_LARGE;
        velk_light_list_way(first, corners, r, b, tri_px, r.size * px_per_world, instance, frame);
        return;
    }
    g_listed_by = VELK_LIGHT_LISTED_VERTICES;
    // A point per vertex, its own or those it links to: a vertex's point is coarse.
    uint words[9];
    uint points[9];
    float weights[9];
    uint owners[9];
    uint read = velk_light_pixel_points(pool, first, corners, b, words, points, weights, owners);
    for (uint k = 0u; k < read; ++k) {
        if (weights[k] > 0.0) {
            velk_light_list_point(pc.pool_word, owners[k], points[k], instance, frame, true);
        }
    }
}

// Lists the one value at word `at`, once a frame, when it is due: the frame's low bits go into
// it, so another pixel finds it listed. Its trace takes a ray a light, each time a first.
void velk_light_list_one(uint at, uint kind, uint target, uint instance, uint table, uint frame)
{
    uint value = light_words.data[at];
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING || !velk_light_one_due(value)) return;
    uint listed = (value & ~(31u << VELK_LIGHT_ONE_QUEUED_SHIFT)) | ((frame & 31u) << VELK_LIGHT_ONE_QUEUED_SHIFT);
    if (listed == value || atomicCompSwap(light_words.data[at], value, listed) != value) return;
    velk_light_trace(kind, target, instance, table, max(pc.lights, 1u));
}

// After the search, the claim again only when it laid records out or made pieces this frame.
bool velk_light_claim_idle()
{
    return pc.again != 0u && light_words.data[pc.pool_word + VELK_POOL_SEARCHED] == 0u
        && light_words.data[pc.pool_word + VELK_POOL_SPLITS] == 0u;
}

// Gives the record `record` that the pixel with ids `ids`, of draw `d`, shows its light, and
// lists the points it reads.
void velk_light_claim(VelkDrawData d, uvec2 ids, uint record)
{
    GlobalData g = velk_globals.data[d.globals_base];
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    uint frame = g.present_counter;

    // A quad: one value on the node over it as wide as the light spacing.
    if (entry == VELK_CUT_QUAD) {
        uint node = velk_light_quad_node(light_words.data[record + VELK_RECORD_QUAD_TREE], light_words.data[record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz,
                                         pixels_per_unit, pc.light_pixels);
        if (light_words.data[node + VELK_QUAD_NODE_LIGHT] == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[node + VELK_QUAD_NODE_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        }
        velk_light_list_one(node + VELK_QUAD_NODE_LIGHT, VELK_LIGHT_TRACE_NODE, node, 0u, 0u, frame);
        return;
    }

    uint was = light_words.data[record + VELK_RECORD_LIGHT];
    if (was == VELK_LIGHT_PENDING) return;
    uint instance = d.instances_base + velk_ids_slot(d, ids);
    // A whole mesh or a shape drawn by its own pass: one value, traced from the mesh's sphere, or
    // a shape without one from its instance's box.
    if (entry == 0xFFFFFFFFu) {
        if (was == VELK_LIGHT_NONE) {
            atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE);
        } else {
            velk_light_list_one(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_MESH, record, instance,
                                light_words.data[record + VELK_RECORD_MESH_TABLE], frame);
        }
        return;
    }
    uint clusters = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    // A cluster: as wide on screen as it is now, and its largest triangle.
    uint cluster = clusters + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];
    mat4 world = velk_claim_world(d, ids);
    float scale = max(max(length(world[0].xyz), length(world[1].xyz)), length(world[2].xyz));
    vec3 own = vec3(uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 1u]),
                    uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 2u]));
    float radius = uintBitsToFloat(light_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
    float away = length(mat3(world) * own + (world[3].xyz - g.cam_pos.xyz));
    float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);

    // Finer detail where a record of coarser detail had points (kLightInherit): its own laid out
    // from those (light_search_src), or a one value when it is small.
    if (velk_light_inherits(was)) {
        if (pc.again != 0u) return;
        if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], was, VELK_LIGHT_PENDING) != was) return;
        if (vertices == 0u || across < pc.light_pixels) {
            light_words.data[record + VELK_RECORD_LIGHT] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_search(record, was, instance);
        return;
    }
    // Pages are laid out once; pieces follow how near the camera is. The points this pixel reads
    // are traced when due, and its record's coarse light.
    if (was != VELK_LIGHT_NONE && was < VELK_LIGHT_ONE_VALUE) {
        if (was >= light_words.data[pc.pool_word + VELK_POOL_PAGES]) return;
        uint first = velk_light_page(pc.pool_word, was);
        velk_light_list_record(was, instance, frame);
        velk_light_list_pixel(first, d, ids, instance, frame, g);
        return;
    }

    // After the search, nothing more goes on the search list: those wait for the next frame.
    if (was == VELK_LIGHT_NONE) {
        if (pc.again != 0u) return;
        if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], VELK_LIGHT_NONE, VELK_LIGHT_PENDING) != VELK_LIGHT_NONE) return;
        if (vertices == 0u || across < pc.light_pixels) {
            light_words.data[record + VELK_RECORD_LIGHT] = VELK_LIGHT_ONE_VALUE;
            return;
        }
        velk_light_search(record, VELK_LIGHT_NONE, instance);
        return;
    }
    // One value, grown to twice the light spacing: points. Else traced from its cluster's sphere.
    if (pc.again != 0u || vertices == 0u || across < 2.0 * pc.light_pixels) {
        velk_light_list_one(record + VELK_RECORD_LIGHT, VELK_LIGHT_TRACE_CLUSTER, record, instance, cluster, frame);
        return;
    }
    if (atomicCompSwap(light_words.data[record + VELK_RECORD_LIGHT], was, VELK_LIGHT_PENDING) != was) return;
    velk_light_search(record, was, instance);
}
