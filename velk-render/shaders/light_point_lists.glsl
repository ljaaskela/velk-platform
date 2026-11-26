// A point list's count (VELK_LIGHT_LIST_*) in the pool's header, and its word's.
uint velk_light_list_count_word(uint list)
{
    return list == VELK_LIGHT_LIST_UNTRACED ? VELK_POOL_UNTRACED_POINTS
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS
                                            : VELK_POOL_FINE_POINTS;
}
uint velk_light_list_word_word(uint list)
{
    return list == VELK_LIGHT_LIST_UNTRACED ? VELK_POOL_UNTRACED_POINTS_WORD
         : list == VELK_LIGHT_LIST_COARSE   ? VELK_POOL_COARSE_POINTS_WORD
                                            : VELK_POOL_FINE_POINTS_WORD;
}

// The header words holding a point list's points traced this frame and its points in their room,
// by its place `k` in velk_points_list's order.
uint velk_points_take_word(uint k)
{
    return VELK_POOL_POINT_TAKES + k;
}
uint velk_points_count_word(uint k)
{
    return VELK_POOL_POINT_COUNTS + k;
}

// The points of light at the corners of a pixel's triangle (corners `corners`, the pixel at
// weights `b` of them) of the record whose first page starts at `first`, up to nine: each
// corner's own point or the three it links to, the words they are in, their numbers there, their
// weights and the first pages of the records they are in, whose lights their slots are. Returns
// how many.
uint velk_light_pixel_points(uint pool, uint first, uvec3 corners, vec3 b, out uint words[9], out uint ids[9],
                             out float weights[9], out uint owners[9])
{
    uint count = 0u;
    for (uint k = 0u; k < 3u; ++k) {
        uint point_words[3];
        uint point_ids[3];
        vec3 w;
        uint owner;
        velk_light_points(pool, first, corners[k], point_words, point_ids, w, owner);
        if (owner == VELK_LIGHT_NONE) continue;
        for (uint j = 0u; j < 3u; ++j) {
            if (point_words[j] == VELK_LIGHT_NONE) continue;
            words[count] = point_words[j];
            ids[count] = point_ids[j];
            weights[count] = b[k] * w[j];
            owners[count] = owner;
            ++count;
        }
    }
    return count;
}
