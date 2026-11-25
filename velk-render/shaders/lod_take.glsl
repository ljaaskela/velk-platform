void velk_lod_take(uint entry, uint c, uint slot, uint cut_record, mat4 world, float scale, vec3 cam, vec3 lod_cam,
                   float pixels_per_unit)
{
    // Drawn in place of finer clusters that are not loaded: its record names a mark, set here
    // where its error as built is too large, which asks for them.
    // Using it also says its own group is wanted, which keeps the group loaded.
    uint taken = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    uint own_mark = cull_words.data[taken + VELK_LOD_CLUSTER_OWN_MARK];
    if (own_mark != 0xFFFFFFFFu) {
        cull_words.data[own_mark] = 1u;
    }
    uint mark = cull_words.data[taken + VELK_LOD_CLUSTER_MARK];
    if (mark != 0xFFFFFFFFu) {
        float from = length(velk_lod_center(taken + VELK_LOD_CLUSTER_SPHERE, world, lod_cam))
                   - uintBitsToFloat(cull_words.data[taken + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale;
        if (uintBitsToFloat(cull_words.data[taken + VELK_LOD_CLUSTER_FULL_ERROR]) * scale * pixels_per_unit / max(from, 1e-3) > pc.lod_max_pixels) {
            cull_words.data[mark] = 1u;
        }
    }
    uint draw = cull_words.data[entry + VELK_CLUSTER_ENTRY_DRAW];
    // Compute raster draws opaque into the visibility buffer: not what a blended batch's own pass draws.
    if (pc.tiny_pixels > 0.0 && draw != 0xFFFFFFFFu
        && (cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & (VELK_LOD_MOVED | VELK_LOD_RAYS_ONLY)) == 0u) {
        uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
        float radius = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_OWN_SPHERE + 3u]) * scale;
        float away = length(velk_lod_center(cluster + VELK_LOD_CLUSTER_OWN_SPHERE, world, cam));
        float across = 2.0 * radius * pixels_per_unit / max(away, 1e-3);
        float triangles = float(max(cull_words.data[cluster + VELK_LOD_CLUSTER_TRIANGLES], 1u));
        // The largest triangle as wide as it can be on screen, at the cluster's nearest.
        float largest = uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_LARGEST]) * scale * pixels_per_unit / max(away - radius, 1e-3);
        if (across < pc.tiny_pixels * sqrt(triangles) && largest < 4.0 * pc.tiny_pixels
            && velk_tiny_add(draw, cluster, slot, cut_record)) {
            return;
        }
    }
    velk_lod_use(entry, c, slot, cut_record);
}
