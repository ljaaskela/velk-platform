// A pixel's way down its triangle's levels of light (velk_piece_way): the triangle's vertex points
// (VELK_PIECE_VERTICES), then its root piece and the pieces under it. The level it reads, the one
// above that and its coarse level, each with the pixel's weights in it.
struct VelkPieceWay {
    uint piece;           // the first level whose points are at most the light spacing apart, or
    uint depth;           // the deepest there is; its depth when a piece
    vec3 at;
    float spacing;        // how far apart its points are on screen, in pixels
    uint root_level;      // the level of the root's grid (velk_piece_level), 0 without a root
    uint parent;          // VELK_PIECE_VERTICES above a root, VELK_LIGHT_NONE above the vertex points
    vec3 parent_at;
    uint coarse;          // the deepest level whose points are at least VELK_LIGHT_COARSE_PIECES
    uint coarse_depth;    // times the light spacing apart, the vertex points when none is
    vec3 coarse_at;
    float coarse_spacing;
};

// Where a pixel or hit is among its record's light roots (velk_light_root_of).
struct VelkLightRoot {
    uint slot;    // its root word's, VELK_LIGHT_NONE when its record has no pieces for its triangle
    vec3 at;      // where it is in the root: its triangle's weights
    float size;   // the root's size in world units: its triangle's longest edge
};

// Where a pixel or hit at weights `b` of triangle `t` (its longest edge `tri_size` world units)
// of the record whose first page starts at `first` is among the record's light roots.
VelkLightRoot velk_light_root_of(uint first, uint t, vec3 b, float tri_size)
{
    VelkLightRoot r;
    r.slot = VELK_LIGHT_NONE;
    r.at = b;
    r.size = tri_size;
    uint pieced = velk_light_pieced(first);
    if (pieced == 0u) return r;
    uint cluster = VELK_LIGHT_WORDS.data[first + VELK_PAGE_CLUSTER];
    if (t >= VELK_LIGHT_WORDS.data[cluster + VELK_LOD_CLUSTER_TRIANGLES]) return r;
    if (t >= pieced) return r;
    r.slot = t;
    return r;
}

// The way down for a pixel at weights `b` of its triangle, whose longest edge is `tri_px` pixels
// long there and whose root piece is `root` (none at or past the pool's pages), to points at most
// `light_pixels` apart. Under the triangle's vertex points the root is at `r` (velk_light_root_of),
// `root_px` pixels across.
VelkPieceWay velk_piece_way(uint pool, uint root, vec3 b, float tri_px, VelkLightRoot r, float root_px, float light_pixels)
{
    uint pages = VELK_LIGHT_WORDS.data[pool + VELK_POOL_PAGES];
    VelkPieceWay w;
    w.piece = VELK_PIECE_VERTICES;
    w.depth = 0u;
    w.at = b;
    w.spacing = tri_px;
    w.root_level = 0u;
    w.parent = VELK_LIGHT_NONE;
    w.parent_at = b;
    w.coarse = VELK_PIECE_VERTICES;
    w.coarse_depth = 0u;
    w.coarse_at = b;
    w.coarse_spacing = tri_px;
    if (root >= pages || tri_px <= light_pixels) return w;
    w.root_level = velk_piece_level(VELK_LIGHT_WORDS.data[velk_light_page(pool, root) + VELK_PIECE_PLACE]);
    w.parent = VELK_PIECE_VERTICES;
    w.piece = root;
    w.at = r.at;
    w.spacing = velk_piece_spacing(0u, w.root_level, root_px);
    for (uint step = 0u; step <= VELK_PIECE_DEEPEST; ++step) {
        if (w.spacing >= VELK_LIGHT_COARSE_PIECES * light_pixels) {
            w.coarse = w.piece;
            w.coarse_depth = w.depth;
            w.coarse_at = w.at;
            w.coarse_spacing = w.spacing;
        }
        if (w.spacing <= light_pixels || w.depth >= VELK_PIECE_DEEPEST) break;
        vec3 l = w.at;
        uint k = velk_piece_step(l);
        uint child = VELK_LIGHT_WORDS.data[velk_light_page(pool, w.piece) + VELK_PIECE_CHILDREN + k];
        if (child >= pages) break;
        w.parent = w.piece;
        w.parent_at = w.at;
        w.piece = child;
        w.at = l;
        ++w.depth;
        w.spacing = velk_piece_spacing(w.depth, w.root_level, root_px);
    }
    return w;
}


// The grid places of the three points of light around weights `w` of a triangle's corners, in
// its grid of level `n`, and their weights. Without a grid, the corners.
void velk_light_around(uint n, vec3 w, out uvec2 places[3], out vec3 weights)
{
    if (n < 2u) {
        places[0] = uvec2(0u, 0u);
        places[1] = uvec2(1u, 0u);
        places[2] = uvec2(0u, 1u);
        weights = w;
        return;
    }
    float a = clamp(w.y, 0.0, 1.0) * float(n);
    float b = clamp(w.z, 0.0, 1.0) * float(n);
    uint i = min(uint(a), n - 1u);
    uint j = min(uint(b), n - 1u - i);
    float fa = clamp(a - float(i), 0.0, 1.0);
    float fb = clamp(b - float(j), 0.0, 1.0);
    if (fa + fb > 1.0 && i + j + 2u <= n) {
        places[0] = uvec2(i + 1u, j + 1u);
        places[1] = uvec2(i, j + 1u);
        places[2] = uvec2(i + 1u, j);
        weights = vec3(fa + fb - 1.0, 1.0 - fa, 1.0 - fb);
    } else {
        float s = max(fa + fb, 1.0);
        places[0] = uvec2(i, j);
        places[1] = uvec2(i + 1u, j);
        places[2] = uvec2(i, j + 1u);
        weights = vec3(1.0 - (fa + fb) / s, fa / s, fb / s);
    }
}

// The three points of the piece at page `page` around weights `l` of it, as words of the pool,
// and their weights.
void velk_piece_around(uint pool, uint page, vec3 l, out uint words[3], out vec3 weights)
{
    uvec2 places[3];
    uint first = velk_light_page(pool, page);
    uint place = VELK_LIGHT_WORDS.data[first + VELK_PIECE_PLACE];
    velk_light_around(velk_piece_level(place), l, places, weights);
    for (uint k = 0u; k < 3u; ++k) words[k] = first + 2u * velk_piece_point(places[k].x, places[k].y);
}

// The vertex of corner `corner` of a triangle of the record whose first page starts at `first`,
// kept within its vertices.
uint velk_way_vertex(uint first, uint corner)
{
    return min(corner, max(VELK_LIGHT_WORDS.data[first + VELK_PAGE_VERTICES], 1u) - 1u);
}

// The three points around weights `l` of level `page` of a pixel's way down (VelkPieceWay) and
// their weights: a piece's, or with VELK_PIECE_VERTICES the vertex points of the triangle with
// corners `corners` in the record whose first page starts at `first`.
void velk_way_around(uint pool, uint first, uvec3 corners, uint page, vec3 l, out uint words[3], out vec3 weights)
{
    if (page != VELK_PIECE_VERTICES) {
        velk_piece_around(pool, page, l, words, weights);
        return;
    }
    for (uint k = 0u; k < 3u; ++k) words[k] = velk_light_own_word(pool, first, velk_way_vertex(first, corners[k]));
    weights = l;
}

// The blocker word of point `k` around a pixel on level `page` of its way down, whose words
// velk_way_around gave as `word`.
uint velk_way_blocker(uint pool, uint first, uvec3 corners, uint page, uint k, uint word)
{
    if (page == VELK_PIECE_VERTICES) return velk_light_blocker_word(pool, first, velk_way_vertex(first, corners[k]));
    return velk_light_page_blocker(pool, page, word - velk_light_page(pool, page));
}


// What the cell of a pixel's coarse level (VelkPieceWay) around it says about the levels under
// it, in the triangle with corners `corners` of the record whose first page starts at `first`:
// 0 when it is not known yet (a point not traced, or with `fresh`, stale since a change or with
// one of its record's lights due), 1 when its three points carry the same light, each light
// fully lit or fully blocked, so that the levels under it could only repeat it: exact; 2 when
// they differ. With `fresh` (the lighting), a due light is no answer: the pixel reads the levels
// under it, which may have traced it since. Without (the listing), what a due light had still
// judges the cell, so that the levels under it are listed while it moves. Without `fresh`, a cell
// whose stale points agree is not exact where a place next to them in its piece has other light:
// an edge could cross into it by the time they are traced again.
uint velk_way_cell(uint pool, uint first, uvec3 corners, VelkPieceWay way, bool fresh)
{
    uint words[3];
    vec3 w;
    velk_way_around(pool, first, corners, way.coarse, way.coarse_at, words, w);
    uint shade = VELK_LIGHT_NONE;
    bool same = true;
    bool stale = false;
    // The record's lights, 4 bits each: a point's blocker code 0 under one is a light due.
    uint lit = 0u;
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        if (VELK_LIGHT_WORDS.data[first + VELK_PAGE_SLOTS + s] != VELK_LIGHT_NONE) lit |= 15u << (4u * s);
    }
    for (uint k = 0u; k < 3u; ++k) {
        uint s = VELK_LIGHT_WORDS.data[words[k]];
        if (s == VELK_LIGHT_NONE) return 0u;
        if (fresh) {
            uint codes = VELK_LIGHT_WORDS.data[velk_way_blocker(pool, first, corners, way.coarse, k, words[k])];
            for (uint l = 0u; l < VELK_LIGHT_SLOTS; ++l) {
                if (((lit >> (4u * l)) & 15u) != 0u && ((codes >> (4u * l)) & 15u) == VELK_LIGHT_BLOCKER_DUE) return 0u;
            }
        }
        uint piece = way.coarse == VELK_PIECE_VERTICES ? VELK_LIGHT_NONE : way.coarse;
        bool old = velk_light_point_traces(pool, first, piece, VELK_LIGHT_WORDS.data[words[k] + 1u]) == 0u;
        if (fresh && old) return 0u;
        stale = stale || old;
        if (k == 0u) shade = s;
        else if (s != shade) same = false;
    }
    // Every light's four bits all clear or all set.
    if (!same || shade != (shade & 0x11111111u) * 15u) return 2u;
    if (fresh || !stale || way.coarse == VELK_PIECE_VERTICES) return 1u;
    uint at = velk_light_page(pool, way.coarse);
    uint place = VELK_LIGHT_WORDS.data[at + VELK_PIECE_PLACE];
    uint n = velk_piece_level(place);
    uvec2 places[3];
    velk_light_around(n, way.coarse_at, places, w);
    // A grid point's neighbours, in the grid's triangles.
    const ivec2 kAround[6] = ivec2[6](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 1));
    for (uint k = 0u; k < 3u; ++k) {
        for (uint a = 0u; a < 6u; ++a) {
            ivec2 q = ivec2(places[k]) + kAround[a];
            if (q.x < 0 || q.y < 0 || q.x + q.y > int(n)) continue;
            uint p = velk_piece_point(uint(q.x), uint(q.y));
            uint s = VELK_LIGHT_WORDS.data[at + 2u * p];
            if (s != VELK_LIGHT_NONE && s != shade) return 2u;
        }
    }
    return 1u;
}

// The way down ended at its coarse level, for a pixel whose coarse cell is exact or, for the
// claim, not known yet: the levels under it are not read.
void velk_way_stop_at_coarse(inout VelkPieceWay w)
{
    w.piece = w.coarse;
    w.depth = w.coarse_depth;
    w.at = w.coarse_at;
    w.spacing = w.coarse_spacing;
    w.parent = VELK_LIGHT_NONE;
    w.parent_at = w.coarse_at;
}

// The first page of the record whose triangle the piece at page `page` is in.
uint velk_piece_record(uint pool, uint page)
{
    return VELK_LIGHT_WORDS.data[velk_light_page(pool, page) + VELK_PIECE_OWNER] & 0xFFFFFFu;
}
