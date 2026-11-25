uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = probe_words.data[grid];
    float size = uintBitsToFloat(probe_words.data[grid + 1u]);
    uint everywhere = probe_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = probe_words.data[b + 1u];
    if (count != 0u) first = grid + probe_words.data[b];
    return everywhere + count;
}
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = probe_words.data[grid + 2u];
    return k < everywhere ? probe_words.data[grid + 4u + k] : probe_words.data[first + k - everywhere];
}
