#define VELK_LIGHT_WORDS velk_light_cell_words
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_SHADOW_SOFT_GLSL
// The soft shadows' second pass, a thread a tile: lists the tile for softening when a light the
// first pass took is kept from some of the tiles within VELK_SHADOW_HALO of it and reaches some, so
// that a penumbra or the blocker search can reach it from an edge. The softening's dispatch is as
// many groups as tiles listed, at most 65535. pc.spare0 is the soft shadows' first word.
void main()
{
    uint base = pc.spare0;
    uint width = velk_light_cell_words.data[base + VELK_SHADOW_WIDTH];
    uint height = velk_light_cell_words.data[base + VELK_SHADOW_HEIGHT];
    uint pixels = width * height;
    int tiles_x = int((width + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE);
    int tiles_y = int((height + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE);
    ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= tiles_x || t.y >= tiles_y) return;
    uint flags = 0u;
    for (int dy = -VELK_SHADOW_HALO; dy <= VELK_SHADOW_HALO; ++dy) {
        int y = t.y + dy;
        if (y < 0 || y >= tiles_y) continue;
        for (int dx = -VELK_SHADOW_HALO; dx <= VELK_SHADOW_HALO; ++dx) {
            int x = t.x + dx;
            if (x < 0 || x >= tiles_x) continue;
            flags |= velk_light_cell_words.data[velk_shadow_tile_word(base, pixels, uint(y * tiles_x + x))];
        }
    }
    if (flags != (VELK_SHADOW_TILE_SHADOW | VELK_SHADOW_TILE_LIT)) return;
    uint tiles = uint(tiles_x * tiles_y);
    uint n = atomicAdd(velk_light_cell_words.data[base + VELK_SHADOW_LISTED], 1u);
    if (n >= tiles) return;
    velk_light_cell_words.data[velk_shadow_list_word(base, pixels, tiles) + n] = uint(t.y * tiles_x + t.x);
    atomicMax(velk_light_cell_words.data[base + VELK_SHADOW_ARGS], min(n + 1u, 65535u));
}
