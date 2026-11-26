
// Point (i, j) of a piece's grid, with i + j at most its level n, at weights
// (1 - (i + j) / n, i / n, j / n) of the piece's corners: row by row in j, each row in i, rows as
// long as a grid of level VELK_PIECE_LEVEL has them, whatever the piece's level.
uint velk_piece_point(uint i, uint j)
{
    return j * (VELK_PIECE_LEVEL + 1u) - (j * (j - 1u)) / 2u + i;
}

// The place (i, j) of point `q` of a piece's grid, as velk_piece_point counts them.
uvec2 velk_piece_place(uint q)
{
    for (uint j = 0u; j <= VELK_PIECE_LEVEL; ++j) {
        uint in_row = VELK_PIECE_LEVEL + 1u - j;
        if (q < in_row) return uvec2(q, j);
        q -= in_row;
    }
    return uvec2(0u, 0u);
}

// A piece's place in its triangle (VELK_PIECE_PLACE): at depth `depth` the triangle is
// 2^depth steps along each edge, its weights (1 - (x + y) / 2^depth, x / 2^depth, y / 2^depth)
// at step (x, y); a piece is at (i, j) with its corners at (i, j), (i + 1, j) and (i, j + 1), or
// turned (`turned`), at (i + 1, j + 1), (i, j + 1) and (i + 1, j). i in bits 0 to 12, j in 13 to 25,
// turned bit 26, the depth in bits 27 to 30, and bit 31 (VELK_PIECE_PLACE_EDGE) for a piece made
// where light differed rather than to bring the levels to the light spacing, which alone may be
// joined back into its parent (light_pieces_src). A root, at depth 0, is at (0, 0) and keeps its
// grid's level in bits 0 to 2 instead (velk_piece_root_place).
#define VELK_PIECE_PLACE_EDGE 0x80000000u

uint velk_piece_place_word(uint depth, uint i, uint j, bool turned)
{
    return (i & 8191u) | ((j & 8191u) << 13u) | (turned ? (1u << 26u) : 0u) | (depth << 27u);
}

// A root's place, with its grid's level `level` (VELK_PIECE_ROOT_LEVELS).
uint velk_piece_root_place(uint level)
{
    return level & 7u;
}

uint velk_piece_depth(uint place)
{
    return (place >> 27u) & 15u;
}

// The level of the grid of the piece at `place`: its own for a root, VELK_PIECE_LEVEL below.
uint velk_piece_level(uint place)
{
    return velk_piece_depth(place) == 0u ? clamp(place & 7u, 2u, VELK_PIECE_LEVEL) : VELK_PIECE_LEVEL;
}

// (i, j) of the piece at `place`; a root's is (0, 0).
uvec2 velk_piece_ij(uint place)
{
    return velk_piece_depth(place) == 0u ? uvec2(0u, 0u) : uvec2(place & 8191u, (place >> 13u) & 8191u);
}

// The corners of the piece at `place`, as steps of its depth (velk_piece_place_word).
void velk_piece_corners(uint place, out ivec2 c[3])
{
    uvec2 ij = velk_piece_ij(place);
    int i = int(ij.x);
    int j = int(ij.y);
    if (velk_piece_depth(place) == 0u || (place & (1u << 26u)) == 0u) {
        c[0] = ivec2(i, j);
        c[1] = ivec2(i + 1, j);
        c[2] = ivec2(i, j + 1);
    } else {
        c[0] = ivec2(i + 1, j + 1);
        c[1] = ivec2(i, j + 1);
        c[2] = ivec2(i + 1, j);
    }
}

// The place of child `k` of the piece at `place`: 0 to 2 the child at that corner, keeping it
// in its place, the other two corners the midpoints of the edges from it; 3 the middle one, its
// corner k the midpoint of the edge across from the piece's corner k.
uint velk_piece_child_place(uint place, uint k)
{
    uint depth = velk_piece_depth(place) + 1u;
    uvec2 ij = velk_piece_ij(place);
    uint i = 2u * ij.x;
    uint j = 2u * ij.y;
    if (depth == 1u || (place & (1u << 26u)) == 0u) {
        if (k == 0u) return velk_piece_place_word(depth, i, j, false);
        if (k == 1u) return velk_piece_place_word(depth, i + 1u, j, false);
        if (k == 2u) return velk_piece_place_word(depth, i, j + 1u, false);
        return velk_piece_place_word(depth, i, j, true);
    }
    if (k == 0u) return velk_piece_place_word(depth, i + 1u, j + 1u, true);
    if (k == 1u) return velk_piece_place_word(depth, i, j + 1u, true);
    if (k == 2u) return velk_piece_place_word(depth, i + 1u, j, true);
    return velk_piece_place_word(depth, i + 1u, j + 1u, false);
}

// Where point (a, b) of the grid of the piece at `place` is in its triangle, as steps of a
// VELK_PIECE_UNITS-th of its depth's steps; every level a grid may have divides that.
ivec2 velk_piece_point_at(uint place, uvec2 ab)
{
    ivec2 c[3];
    velk_piece_corners(place, c);
    int step = int(VELK_PIECE_UNITS / velk_piece_level(place));
    return int(VELK_PIECE_UNITS) * c[0] + step * (int(ab.x) * (c[1] - c[0]) + int(ab.y) * (c[2] - c[0]));
}

// The weights in its triangle of point (a, b) of the grid of the piece at `place`.
vec3 velk_piece_weights(uint place, uvec2 ab)
{
    ivec2 x = velk_piece_point_at(place, ab);
    float steps = float(VELK_PIECE_UNITS) * exp2(float(velk_piece_depth(place)));
    return vec3(steps - float(x.x + x.y), float(x.x), float(x.y)) / steps;
}

// The point of the grid of the piece at `place` at `x` (as velk_piece_point_at counts), or
// VELK_LIGHT_NONE when it has none there.
uint velk_piece_point_of(uint place, ivec2 x)
{
    ivec2 c[3];
    velk_piece_corners(place, c);
    uint n = velk_piece_level(place);
    int step = int(VELK_PIECE_UNITS / n);
    ivec2 r = x - int(VELK_PIECE_UNITS) * c[0];
    int a = r.x * (c[1].x - c[0].x);
    int b = r.y * (c[2].y - c[0].y);
    if (a < 0 || b < 0 || a % step != 0 || b % step != 0) return VELK_LIGHT_NONE;
    a /= step;
    b /= step;
    if (a + b > int(n)) return VELK_LIGHT_NONE;
    return velk_piece_point(uint(a), uint(b));
}

// How far apart on screen, in pixels, the points of a piece at `depth` are, in a triangle whose
// longest edge is `tri_px` pixels long where the pixel is and whose root's grid is of level
// `root_level`.
float velk_piece_spacing(uint depth, uint root_level, float tri_px)
{
    uint n = depth == 0u ? root_level : VELK_PIECE_LEVEL;
    return tri_px / (float(n) * exp2(float(depth)));
}

// The level of a root's grid for a triangle whose longest edge is `tri_px` pixels long, its points
// about `light_pixels` apart: one of VELK_PIECE_ROOT_LEVELS. A triangle at least three times
// VELK_LIGHT_COARSE_PIECES times that long gets the coarsest grid whose points are still that far
// apart, so that its root is its coarse level, and not its vertices. Not from twice: as many
// roots more cost moving lights about a third more trace.
uint velk_piece_root_level(float tri_px, float light_pixels)
{
    float coarse = VELK_LIGHT_COARSE_PIECES * light_pixels;
    if (tri_px >= 3.0 * coarse) {
        uint n = uint(clamp(floor(tri_px / coarse), 2.0, float(VELK_PIECE_LEVEL)));
        return n == 5u ? 4u : n;
    }
    uint n = uint(clamp(ceil(tri_px / light_pixels), 2.0, float(VELK_PIECE_LEVEL)));
    return n == 5u ? VELK_PIECE_LEVEL : n;
}

// Which child of a piece weights `l` of it are in, as velk_piece_child_place numbers them; `l`
// becomes the weights in that child.
uint velk_piece_step(inout vec3 l)
{
    uint k = 3u;
    if (l.x >= 0.5) {
        l = vec3(2.0 * l.x - 1.0, 2.0 * l.y, 2.0 * l.z);
        k = 0u;
    } else if (l.y >= 0.5) {
        l = vec3(2.0 * l.x, 2.0 * l.y - 1.0, 2.0 * l.z);
        k = 1u;
    } else if (l.z >= 0.5) {
        l = vec3(2.0 * l.x, 2.0 * l.y, 2.0 * l.z - 1.0);
        k = 2u;
    } else {
        l = vec3(1.0) - 2.0 * l;
    }
    l = clamp(l, 0.0, 1.0);
    l /= max(l.x + l.y + l.z, 1e-6);
    return k;
}
