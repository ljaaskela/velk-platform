void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    // The scene's lights, for what the next frame's mark lists (light_mark_src).
    if (coord.x == 0u && coord.y == 0u && pc.again == 0u) light_words.data[pc.pool_word + VELK_POOL_LIGHTS] = pc.lights;
    if (coord.x >= pc.width || coord.y >= pc.height) return;
    if (velk_light_claim_idle()) return;
    g_coord = coord;
    uvec2 ids = velk_pixel_ids(pc.ids_word, pc.width, ivec2(coord));
    if (ids.x == 0u) return;
    VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
    uint record = velk_ids_record(d, ids);
    if (record == VELK_NO_RECORD) return;
    velk_light_claim(d, ids, record);
}
