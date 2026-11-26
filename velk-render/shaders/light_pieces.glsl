#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer LightWords { uint data[]; } light_words;

layout(push_constant) uniform PC {
    uint pool_word;
    uint phase;
} pc;

#define VELK_LIGHT_WORDS light_words
#velk_include VELK_LIGHT_POOL_GLSL
bool velk_pieces_marked(uint pool, uint page)
{
    return (light_words.data[light_words.data[pool + VELK_POOL_MARKS_WORD] + (page >> 5u)] & (1u << (page & 31u))) != 0u;
}

// Frames an unread piece is kept: VELK_PIECE_KEEP_FRAMES when the pool is short of pages, longer
// the more room it has, so that turning back to what went out of view finds its pieces and their
// light. It shortens a step at a time as the pool fills, so that a burst of new pieces while the
// camera turns does not let every piece out of view go at once. The room is what the last frame
// left free: this runs before the sweep refills the stack. A pool asked for pages it did not have
// keeps them VELK_PIECE_KEEP_FRAMES, so that what out-of-view pieces were let keep goes to what is
// in view before the pool grows; one that cannot grow, half as long for each such frame in a row,
// down to an eighth, so that the pieces read longest ago make room for those in view.
uint velk_pieces_keep_frames(uint pool)
{
    uint free_pages = light_words.data[pool + VELK_POOL_LAST_FREE_PAGES];
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    uint demand = light_words.data[pool + VELK_POOL_DEMAND_FRAMES];
    // The free stack has room for the most pages the pool may have: all of them are there.
    bool at_most = pages == light_words.data[pool + VELK_POOL_MARKS_WORD] - light_words.data[pool + VELK_POOL_STACK_WORD];
    if (demand > 0u) return at_most ? VELK_PIECE_KEEP_FRAMES >> min(demand, 3u) : VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 2u > pages) return 32u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 4u > pages) return 8u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 8u > pages) return 4u * VELK_PIECE_KEEP_FRAMES;
    if (free_pages * 16u > pages) return 2u * VELK_PIECE_KEEP_FRAMES;
    return VELK_PIECE_KEEP_FRAMES;
}

// Whether the piece starting at word `at`, which has no children, has not been read for `keep`
// frames, the pool's frame being `now`.
bool velk_pieces_unread(uint at, uint now, uint keep)
{
    uint read = light_words.data[at + VELK_PIECE_READ] & VELK_PIECE_FRAME_BITS;
    return ((now - read) & VELK_PIECE_FRAME_BITS) > keep;
}

// Whether the piece starting at word `at` and its four children all carry the same light at
// every point of their grids, each light fully through or fully blocked, as their points'
// stored light says whether stale or not; a point not traced yet says no.
bool velk_pieces_same(uint pool, uint at, uint pages)
{
    uint shade = light_words.data[at];
    if (shade == VELK_LIGHT_NONE || shade != (shade & 0x11111111u) * 15u) return false;
    uint place = light_words.data[at + VELK_PIECE_PLACE];
    uint level = velk_piece_level(place);
    for (uint q = 1u; q < VELK_PIECE_POINTS; ++q) {
        uvec2 ab = velk_piece_place(q);
        if (ab.x + ab.y <= level && light_words.data[at + 2u * q] != shade) return false;
    }
    for (uint k = 0u; k < 4u; ++k) {
        uint child = light_words.data[at + VELK_PIECE_CHILDREN + k];
        if (child >= pages) return false;
        uint child_at = velk_light_page(pool, child);
        for (uint q = 0u; q < VELK_PIECE_POINTS; ++q) {
            if (light_words.data[child_at + 2u * q] != shade) return false;
        }
    }
    return true;
}

void main()
{
    uint pool = pc.pool_word;
    uint page = gl_GlobalInvocationID.x;
    uint pages = light_words.data[pool + VELK_POOL_PAGES];
    if (page >= pages || !velk_light_is_piece(pool, page)) return;
    uint at = velk_light_page(pool, page);
    uint owner = light_words.data[at + VELK_PIECE_OWNER];
    uint record = owner & 0xFFFFFFu;
    uint parent = light_words.data[at + VELK_PIECE_PARENT];
    uint now = light_words.data[pool + VELK_POOL_FRAME] & VELK_PIECE_FRAME_BITS;
    bool live = record < pages && velk_pieces_marked(pool, record);
    uint first = live ? velk_light_page(pool, record) : 0u;
    uint triangle = owner >> 24u;
    if (live && triangle >= velk_light_pieced(first)) live = false;

    if (pc.phase == 0u) {
        if (!live) return;
        uint keep = velk_pieces_keep_frames(pool);
        uint child0 = light_words.data[at + VELK_PIECE_CHILDREN];
        if (child0 < pages) {
            // Its four children, when none has children and none was read, or when none has
            // children and their light has been the same as its own for long enough.
            bool unread = true;
            bool leaves = true;
            for (uint k = 0u; k < 4u; ++k) {
                uint child = light_words.data[at + VELK_PIECE_CHILDREN + k];
                if (child >= pages) {
                    unread = false;
                    leaves = false;
                    break;
                }
                uint child_at = velk_light_page(pool, child);
                if (light_words.data[child_at + VELK_PIECE_CHILDREN] < pages) {
                    unread = false;
                    leaves = false;
                } else if (!velk_pieces_unread(child_at, now, keep)) {
                    unread = false;
                }
            }
            bool merge = false;
            // Only children made where light differed (VELK_PIECE_PLACE_EDGE): those made to bring
            // the levels to the light spacing would be asked for again at once.
            bool edge = (light_words.data[velk_light_page(pool, child0) + VELK_PIECE_PLACE] & VELK_PIECE_PLACE_EDGE) != 0u;
            if (!unread && leaves && edge) {
                uint read = light_words.data[at + VELK_PIECE_READ];
                uint count = read >> VELK_PIECE_SAME_SHIFT;
                if (velk_pieces_same(pool, at, pages)) {
                    merge = count >= VELK_PIECE_SAME_FRAMES;
                    count = merge ? 0u : count + 1u;
                } else {
                    count = 0u;
                }
                uint now_read = (read & ~VELK_PIECE_SAME_BITS) | (count << VELK_PIECE_SAME_SHIFT);
                if (now_read != read) light_words.data[at + VELK_PIECE_READ] = now_read;
            }
            if (unread || merge) {
                for (uint k = 0u; k < 4u; ++k) light_words.data[at + VELK_PIECE_CHILDREN + k] = VELK_LIGHT_NONE;
                atomicAdd(light_words.data[pool + (unread ? VELK_POOL_SUM_PIECES_UNREAD : VELK_POOL_SUM_PIECES_MERGED)], 4u);
            }
        } else if (parent == VELK_LIGHT_NONE && velk_pieces_unread(at, now, keep)) {
            if (atomicCompSwap(light_words.data[velk_light_root_word(pool, first, triangle)], page, VELK_LIGHT_NONE) == page) {
                atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_UNREAD], 1u);
            }
        }
        return;
    }

    // Still linked: from its parent, itself a piece of the same record, or as its record's root.
    bool linked = false;
    if (live && parent == VELK_LIGHT_NONE) {
        linked = light_words.data[velk_light_root_word(pool, first, triangle)] == page;
    } else if (live && parent < pages && velk_light_is_piece(pool, parent)) {
        uint parent_at = velk_light_page(pool, parent);
        if ((light_words.data[parent_at + VELK_PIECE_OWNER] & 0xFFFFFFu) == record) {
            for (uint k = 0u; k < 4u; ++k) linked = linked || light_words.data[parent_at + VELK_PIECE_CHILDREN + k] == page;
        }
    }
    if (!linked) {
        atomicAdd(light_words.data[pool + VELK_POOL_SUM_PIECES_FREED], 1u);
        return;
    }
    atomicOr(light_words.data[light_words.data[pool + VELK_POOL_MARKS_WORD] + (page >> 5u)], 1u << (page & 31u));
    atomicAdd(light_words.data[pool + VELK_POOL_MARK_PIECES], 1u);
    atomicMax(light_words.data[pool + VELK_POOL_MARK_DEEPEST], velk_piece_depth(light_words.data[at + VELK_PIECE_PLACE]));
}
