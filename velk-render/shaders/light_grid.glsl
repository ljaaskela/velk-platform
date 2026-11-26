uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// How many lights could reach the place `world_at`, from the world's origin, and in `first`
// where its bucket's lights start, for velk_light_grid_at. Every light if there is no grid.
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = velk_light_cell_words.data[grid];
    float size = uintBitsToFloat(velk_light_cell_words.data[grid + 1u]);
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = velk_light_cell_words.data[b + 1u];
    if (count != 0u) first = grid + velk_light_cell_words.data[b];
    return everywhere + count;
}

// The `k`th light that could reach the place velk_light_grid_count gave `first` for.
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = velk_light_cell_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = velk_light_cell_words.data[grid + 2u];
    return k < everywhere ? velk_light_cell_words.data[grid + 4u + k] : velk_light_cell_words.data[first + k - everywhere];
}
