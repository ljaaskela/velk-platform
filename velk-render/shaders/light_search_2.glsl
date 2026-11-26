// The most triangles a cluster has (kClusterMaxTriangles).
const uint kMaxTriangles = 256u;

shared bool s_own;          // it has its own points, which all threads make untraced
shared uint s_page;         // then its first page
shared uint s_first;        // and that page's word
shared uint s_table;        // its triangles' root words, 0 for none
shared uint s_points;

// Takes `count` pages off the free stack; the kth is velk_light_taken(pool, base, k). False when
// there are not that many. The count of pages taken only ever moves forward, past a take that
// fits: a take that does not leaves it as it is, so that no two takes get the same pages. A
// swap that fails means another take moved it, so there are at most as many tries as takes.
bool velk_light_take(uint pool, uint count, out uint base)
{
    uint free_pages = light_words.data[pool + VELK_POOL_FREE_PAGES];
    uint taken = light_words.data[pool + VELK_POOL_PAGES_TAKEN];
    base = 0u;
    for (uint tries = 0u; tries < 4096u; ++tries) {
        if (taken + count > free_pages) return false;
        uint seen = atomicCompSwap(light_words.data[pool + VELK_POOL_PAGES_TAKEN], taken, taken + count);
        if (seen == taken) {
            base = taken;
            return true;
        }
        taken = seen;
    }
    return false;
}

uint velk_light_taken(uint pool, uint base, uint k)
{
    return light_words.data[light_words.data[pool + VELK_POOL_STACK_WORD] + light_words.data[pool + VELK_POOL_FREE_PAGES] - 1u - base - k];
}

// Pages for a record of `vertices` vertices of `cluster` (its word in its hierarchy's table), as
// `kind`, with `table` words after its header and `points` point words: its header written and
// its pages named. Its points are left to the caller. False, and the pages asked for, when there
// is no room.
bool velk_light_lay_out(uint vertices, uint cluster, uint kind, uint table, uint points, uint index_base, out uint first_page)
{
    uint pool = pc.pool_word;
    uint count = (velk_light_skip_for(table) + points + VELK_LIGHT_PAGE_WORDS - 1u) / VELK_LIGHT_PAGE_WORDS;
    first_page = VELK_LIGHT_NONE;
    if (count > VELK_LIGHT_DIRECT_PAGES) return false;
    uint base;
    if (!velk_light_take(pool, count, base)) {
        atomicAdd(light_words.data[pool + VELK_POOL_PAGES_WANTED], count);
        return false;
    }
    uint page0 = velk_light_taken(pool, base, 0u);
    uint first = velk_light_page(pool, page0);
    light_words.data[first + VELK_PAGE_VERTICES] = vertices;
    light_words.data[first + VELK_PAGE_KIND] = kind;
    light_words.data[first + VELK_PAGE_POINT_WORDS] = points;
    for (uint k = 1u; k < VELK_LIGHT_DIRECT_PAGES; ++k) {
        light_words.data[first + VELK_PAGE_PAGES - 1u + k] = k < count ? velk_light_taken(pool, base, k) : VELK_LIGHT_NONE;
    }
    light_words.data[first + VELK_PAGE_CHANGED] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_CLUSTER] = cluster;
    light_words.data[first + VELK_PAGE_RESET] = VELK_LIGHT_NONE;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) light_words.data[first + VELK_PAGE_SLOTS + s] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_INDEX] = index_base;
    light_words.data[first + VELK_PAGE_FIRST] = page0;
    light_words.data[first + VELK_PAGE_TOUCHED] = VELK_LIGHT_NONE;
    light_words.data[first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE;
    light_words.data[first + VELK_PAGE_READ] = 0u;
    first_page = page0;
    return true;
}

// Puts the coarse light of the own points just laid out at `page` on the trace list, so that it
// is traced in the frame they are laid out; their points are listed as pixels read them.
void velk_light_trace_now(uint page, uint instance)
{
    uint frame = velk_globals.data[pc.globals_base].present_counter;
    velk_light_list_coarse(pc.pool_word, page, instance, frame, pc.lights);
}


// Making pieces (the split list), a work group an ask: the new pieces are the levels `s_from` to
// `s_to` under the top piece (level 0), every level whole, 4^r pieces at level r, taken off the
// free stack in that order from `s_base`. The top piece is new for a root (s_from 0), else it is
// the piece asked for, `s_top`.
shared uint s_split;        // 0 for nothing to make, 1 for a root, 2 for children
shared uint s_top;          // the top piece's page: the piece asked for, or the new root
shared uint s_top_place;
shared uint s_from;
shared uint s_to;
shared uint s_base;
shared uint s_record;       // the record's first page, and its first page's word
shared uint s_record_first;
shared uint s_triangle;
shared uint s_root_word;
shared uint s_path;         // the asking pixel's way down (VELK_SPLIT_PATH_SHIFT), 2 bits a level
shared uint s_edge;         // VELK_PIECE_PLACE_EDGE for pieces asked where light differs, else 0

// Which child of the path's piece at level `level` (1 and on) the path goes on in: the one whose
// four children make the next level.
uint velk_split_digit(uint level)
{
    return (s_path >> (2u * (level - 1u))) & 3u;
}

// The pieces made: the root at level 0 when it is new, then at each level the four children of the
// path's piece of the level above.
uint velk_split_count()
{
    return (s_from == 0u ? 1u : 0u) + 4u * s_to;
}

// The page of piece `q` (0 to 3, the child of the path's piece above it) at level `level` of the
// pieces being made, or the top piece at level 0.
uint velk_split_page(uint level, uint q)
{
    if (level == 0u) return s_top;
    return velk_light_taken(pc.pool_word, s_base, (s_from == 0u ? 1u : 0u) + 4u * (level - 1u) + q);
}

// The place of piece `q` at level `level` under the top piece: down the path to the level above,
// then its child `q`.
uint velk_split_place(uint level, uint q)
{
    if (level == 0u) return s_top_place;
    uint place = s_top_place;
    for (uint r = 1u; r < level && r <= VELK_PIECE_LEVELS_AT_ONCE; ++r) {
        place = velk_piece_child_place(place, velk_split_digit(r));
    }
    return velk_piece_child_place(place, q);
}

// Writes piece `q` at level `level` of the pieces being made, its points untraced but for a root's
// corners, which the vertices' points give. A child traces even the points it shares with its
// parent at its own places, and the parent shows under it until they are traced.
void velk_split_write(uint level, uint q, uint frame)
{
    uint pool = pc.pool_word;
    uint page = velk_split_page(level, q);
    uint at = velk_light_page(pool, page);
    uint place = velk_split_place(level, q);
    // Children only on the path: all four or none.
    bool on_path = level < s_to && (level == 0u || q == velk_split_digit(level));
    for (uint k = 0u; k < 4u; ++k) {
        light_words.data[at + VELK_PIECE_CHILDREN + k] = on_path ? velk_split_page(level + 1u, k) : VELK_LIGHT_NONE;
    }
    light_words.data[at + VELK_PIECE_PLACE] = place | s_edge;
    light_words.data[at + VELK_PIECE_OWNER] = s_record | (s_triangle << 24u);
    light_words.data[at + VELK_PIECE_PARENT] = level == 0u ? VELK_LIGHT_NONE
                                             : velk_split_page(level - 1u, level == 1u ? 0u : velk_split_digit(level - 1u));
    light_words.data[at + VELK_PIECE_READ] = frame;
    for (uint p = 0u; p < VELK_PIECE_POINTS; ++p) {
        uint shade = VELK_LIGHT_NONE;
        uint state = VELK_LIGHT_NONE;
        uint blockers = 0u;
        uvec2 ab = velk_piece_place(p);
        uint from_word = VELK_LIGHT_NONE;
        uint from_blocker = VELK_LIGHT_NONE;
        if (level == 0u && s_from == 0u) {
            // A root's corners are its triangle's vertices.
            uint n = velk_piece_level(place);
            uint corner = ab == uvec2(0u, 0u) ? 0u : ab == uvec2(n, 0u) ? 1u : ab == uvec2(0u, n) ? 2u : 3u;
            uint index = light_words.data[s_record_first + VELK_PAGE_INDEX];
            uint cluster = light_words.data[s_record_first + VELK_PAGE_CLUSTER];
            uint triangle = s_triangle;
            if (corner < 3u && index != VELK_LIGHT_NONE && triangle < light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) {
                uint v = velk_mesh_words.data[index + light_words.data[cluster + VELK_LOD_CLUSTER_INDEX] + triangle * 3u + corner];
                if (v < light_words.data[s_record_first + VELK_PAGE_VERTICES]) {
                    from_word = velk_light_own_word(pool, s_record_first, v);
                    from_blocker = velk_light_blocker_word(pool, s_record_first, v);
                }
            }
        }
        // Not a stale vertex point's: the root's own changes start from none, so it would read
        // as fresh.
        if (from_word != VELK_LIGHT_NONE
            && velk_light_point_traces(pool, s_record_first, VELK_LIGHT_NONE, light_words.data[from_word + 1u]) > 0u) {
            shade = light_words.data[from_word];
            state = light_words.data[from_word + 1u];
            blockers = light_words.data[from_blocker];
        }
        light_words.data[at + 2u * p] = shade;
        light_words.data[at + 2u * p + 1u] = state;
        light_words.data[velk_light_page_blocker(pool, page, 2u * p)] = blockers;
    }
    // Nothing has changed in its way yet.
    light_words.data[velk_piece_changed_word(pool, page)] = VELK_LIGHT_NONE;
    // A piece now, for the pool's sweep.
    atomicOr(light_words.data[light_words.data[pool + VELK_POOL_PIECES_WORD] + (page >> 5u)], 1u << (page & 31u));
}

// Makes the pieces split list entry `index` asks for, all threads of the group together.
void velk_split(uint index)
{
    uint pool = pc.pool_word;
    uint lane = gl_LocalInvocationIndex;
    uint frame = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    if (lane == 0u) {
        s_split = 0u;
        uint pages = light_words.data[pool + VELK_POOL_PAGES];
        uint w = light_words.data[pool + VELK_POOL_SPLIT_WORD] + index * VELK_SPLIT_ENTRY_WORDS;
        uint page = light_words.data[w + VELK_SPLIT_ENTRY_PAGE];
        uint how = light_words.data[w + VELK_SPLIT_ENTRY_HOW];
        uint depth = min((how >> 8u) & 255u, VELK_PIECE_DEEPEST);
        bool root = (how & VELK_LIGHT_SPLIT_ROOT) != 0u;
        s_path = (how >> VELK_SPLIT_PATH_SHIFT) & 0xFFu;
        s_edge = (how & VELK_LIGHT_SPLIT_EDGE) != 0u ? VELK_PIECE_PLACE_EDGE : 0u;
        if (page < pages) {
            if (root) {
                // The root word is pending while the ask is on the list: it is this group's.
                s_record = page;
                s_record_first = velk_light_page(pool, page);
                s_triangle = how & 255u;
                if (s_triangle < velk_light_pieced(s_record_first)) {
                    s_root_word = velk_light_root_word(pool, s_record_first, s_triangle);
                    if (light_words.data[s_root_word] == VELK_LIGHT_PENDING) {
                        s_split = 1u;
                        s_from = 0u;
                        s_to = min(depth, VELK_PIECE_LEVELS_AT_ONCE);
                        uint level = (how >> 16u) & 7u;
                        s_top_place = velk_piece_root_place(level == 0u ? VELK_PIECE_LEVEL : level);
                    }
                }
            } else if (velk_light_is_piece(pool, page)) {
                uint top = velk_light_page(pool, page);
                uint top_place = light_words.data[top + VELK_PIECE_PLACE];
                uint top_depth = velk_piece_depth(top_place);
                uint owner = light_words.data[top + VELK_PIECE_OWNER];
                if (light_words.data[top + VELK_PIECE_CHILDREN] >= pages && top_depth < VELK_PIECE_DEEPEST
                    && (owner & 0xFFFFFFu) < pages) {
                    s_split = 2u;
                    s_top = page;
                    s_top_place = top_place;
                    s_record = owner & 0xFFFFFFu;
                    s_record_first = velk_light_page(pool, s_record);
                    s_triangle = owner >> 24u;
                    s_from = 1u;
                    s_to = clamp(depth, top_depth + 1u, min(top_depth + VELK_PIECE_LEVELS_AT_ONCE, VELK_PIECE_DEEPEST)) - top_depth;
                }
            }
        }
        if (s_split != 0u) {
            uint count = velk_split_count();
            uint base;
            if (velk_light_take(pool, count, base)) {
                s_base = base;
                if (s_split == 1u) s_top = velk_light_taken(pool, base, 0u);
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_MADE], count);
            } else {
                atomicAdd(light_words.data[pool + VELK_POOL_PAGES_WANTED], count);
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_SPLITS_REFUSED], 1u);
                if (s_split == 1u) light_words.data[s_root_word] = VELK_LIGHT_NONE;
                s_split = 0u;
            }
        } else if (root && page < pages) {
            // Not this group's to make after all: asked again when read.
            uint first = velk_light_page(pool, page);
            if ((how & 255u) < velk_light_pieced(first)) {
                uint word = velk_light_root_word(pool, first, how & 255u);
                if (light_words.data[word] == VELK_LIGHT_PENDING) light_words.data[word] = VELK_LIGHT_NONE;
            }
        }
    }
    memoryBarrierShared();
    barrier();
    if (s_split == 0u) return;
    // All at once: no piece takes anything from another.
    uint made = velk_split_count();
    uint root_made = s_from == 0u ? 1u : 0u;
    for (uint i = lane; i < made; i += 64u) {
        if (i < root_made) {
            velk_split_write(0u, 0u, frame);
        } else {
            velk_split_write((i - root_made) / 4u + 1u, (i - root_made) % 4u, frame);
        }
    }
    memoryBarrierBuffer();
    barrier();
    // Then linked in, for the passes after this one.
    if (lane == 0u) {
        if (s_split == 1u) {
            light_words.data[s_root_word] = s_top;
        } else {
            uint top = velk_light_page(pool, s_top);
            for (uint k = 0u; k < 4u; ++k) light_words.data[top + VELK_PIECE_CHILDREN + k] = velk_split_page(1u, k);
        }
    }
}


void main()
{
    uint pool = pc.pool_word;
    uint index = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    // Past the search list's groups, the split list's.
    if (index >= VELK_LIGHT_SEARCH_CAPACITY) {
        uint split = index - VELK_LIGHT_SEARCH_CAPACITY;
        if (split < min(light_words.data[pool + VELK_POOL_SPLITS], VELK_LIGHT_SPLIT_CAPACITY)) velk_split(split);
        return;
    }
    if (index >= light_words.data[pool + VELK_POOL_SEARCHED]) return;
    uint list = light_words.data[pool + VELK_POOL_SEARCH_WORD] + index * VELK_SEARCH_ENTRY_WORDS;
    uint record = light_words.data[list + VELK_SEARCH_ENTRY_RECORD];
    uint was = light_words.data[list + VELK_SEARCH_ENTRY_WAS];
    uint instance = light_words.data[list + VELK_SEARCH_ENTRY_INSTANCE];
    uint entry = light_words.data[record + VELK_RECORD_ENTRY];
    uint clusters = light_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint cluster = clusters + light_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    uint vertices = light_words.data[cluster + VELK_LOD_CLUSTER_VERTICES];

    if (lane == 0u) {
        s_own = false;
        uint draw = light_words.data[entry + VELK_CLUSTER_ENTRY_DRAW];
        // Seen from both sides, its points take light from either.
        uint kind_flags = draw != 0xFFFFFFFFu && (velk_draw_data.data[draw].flags & VELK_DRAW_DOUBLE_SIDED) != 0u
                        ? VELK_LIGHT_TWO_SIDED : 0u;
        // Its own points, a point a vertex, and with its draw's indices to find its triangles'
        // corners, a root word a triangle, its pieces made as pixels read them. All threads make
        // them untraced and without roots, below.
        uint index_base = draw == 0xFFFFFFFFu ? VELK_LIGHT_NONE : velk_draw_data.data[draw].index_base;
        uint triangles = min(light_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], kMaxTriangles);
        uint table = index_base != VELK_LIGHT_NONE ? triangles : 0u;
        uint kind = (table != 0u ? VELK_LIGHT_PIECED | triangles : VELK_LIGHT_OWN) | kind_flags;
        uint laid;
        if (velk_light_lay_out(vertices, cluster, kind, table, 2u * vertices, index_base, laid)) {
            s_page = laid;
            s_first = velk_light_page(pool, laid);
            s_table = table;
            s_points = vertices;
            s_own = true;
        } else {
            light_words.data[record + VELK_RECORD_LIGHT] = was;
        }
    }
    memoryBarrierBuffer();
    barrier();
    // Its own points: its triangles without roots and the points untraced, then the record names
    // them and they are traced this frame.
    if (s_own) {
        for (uint t = lane; t < s_table; t += 64u) {
            light_words.data[velk_light_root_word(pool, s_first, t)] = VELK_LIGHT_NONE;
        }
        for (uint p = lane; p < s_points; p += 64u) {
            uint at = velk_light_own_word(pool, s_first, p);
            light_words.data[at] = VELK_LIGHT_NONE;
            light_words.data[at + 1u] = VELK_LIGHT_NONE;
            light_words.data[velk_light_blocker_word(pool, s_first, p)] = 0u;
        }
        memoryBarrierBuffer();
        barrier();
        if (lane == 0u) {
            // Grown from a traced one value: its coarse light starts as that one value, from the
            // same sphere and lights (velk_light_seeded), so that its pixels show what they showed
            // until its points are traced.
            if (was >= VELK_LIGHT_ONE_VALUE && was < VELK_LIGHT_PENDING && (was & VELK_LIGHT_ONE_TRACED) != 0u) {
                light_words.data[s_first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE | (was & VELK_LIGHT_ONE_SHADE_BITS);
            }
            // Finer detail where a record of coarser detail had points (kLightInherit): its lights
            // and its coarse light, chosen and traced around the same place, which the light mark
            // kept for this.
            uint parent = was & ~VELK_LIGHT_INHERIT;
            if (velk_light_inherits(was) && parent < light_words.data[pool + VELK_POOL_PAGES]) {
                uint parent_first = velk_light_page(pool, parent);
                uint coarse = light_words.data[parent_first + VELK_PAGE_COARSE];
                if (light_words.data[parent_first + VELK_PAGE_SLOTS] != VELK_LIGHT_NONE && coarse >= VELK_LIGHT_ONE_VALUE
                    && coarse < VELK_LIGHT_PENDING && (coarse & VELK_LIGHT_ONE_TRACED) != 0u) {
                    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
                        light_words.data[s_first + VELK_PAGE_SLOTS + s] = light_words.data[parent_first + VELK_PAGE_SLOTS + s];
                    }
                    light_words.data[s_first + VELK_PAGE_COARSE] = VELK_LIGHT_ONE_VALUE | (coarse & VELK_LIGHT_ONE_SHADE_BITS);
                }
            }
            light_words.data[record + VELK_RECORD_LIGHT] = s_page;
            velk_light_trace_now(s_page, instance);
            // For the log, why: no light before, one value before, or a coarser record's points.
            uint why = was == VELK_LIGHT_NONE ? VELK_POOL_SUM_LAID_NEW
                     : velk_light_inherits(was) ? VELK_POOL_SUM_LAID_INHERITED : VELK_POOL_SUM_LAID_FROM_ONE;
            atomicAdd(light_words.data[pool + why], s_points);
        }
    }
}
