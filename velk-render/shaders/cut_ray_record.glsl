// Adds `record`'s share to the partition's box and counts: none for one for rays only
// (kCutForRays) or a patch for rays, which are not drawn; a deforming mesh's or an item drawn
// by its own pass makes the partition always visited.
void velk_draw_share(uint record, inout vec3 lo, inout vec3 hi, inout uint always, inout uint for_rays, inout uint quads)
{
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (velk_cut_for_rays(entry)) {
        ++for_rays;
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD) return;
    if (entry == VELK_CUT_QUAD) {
        ++quads;
    } else if (entry != 0xFFFFFFFFu) {
        uint flags = cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS];
        if ((flags & VELK_LOD_DEFORMING) != 0u) always = 1u;
    } else if (pc.items_word != 0xFFFFFFFFu) {
        uint item = pc.items_word + cull_words.data[record + VELK_RECORD_SLOT] * VELK_PLAIN_ITEM_WORDS;
        if (cull_words.data[item + VELK_PLAIN_ITEM_TABLE] == 0xFFFFFFFFu) always = 1u;
    }
    vec3 rlo, rhi;
    velk_cut_record_box(record, pc.instances_base, pc.items_word, pc.pad, rlo, rhi);
    lo = min(lo, rlo);
    hi = max(hi, rhi);
}

// How wide on screen, from `from`, the error of the sphere at `word` (centre, radius, error) is.
float velk_ray_pixels(uint word, mat4 world, float scale, vec3 from, float pixels_per_unit)
{
    vec3 center = mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                                     uintBitsToFloat(cull_words.data[word + 2u])) + world[3].xyz - from;
    float away = length(center) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// Whether rays trace `record`, the record at `place` of the pool: at the light's level, seen
// from `viewpoint` where its partition chose. A record for rays only is, and a patch for rays;
// a record of a member in a patch is not, the patch standing for it; a cluster drawn finer than
// the light, whose parent is not coarser than the light, is not, since a record for rays only
// covers it.
bool velk_ray_record(uint record, uint place, vec3 viewpoint, float pixels_per_unit)
{
    uint marks = cull_words.data[pc.cut_word + VELK_CUT_RAY_MARKS_WORD];
    if ((cull_words.data[marks + (place >> 5u)] & (1u << (place & 31u))) != 0u) return false;
    uint entry = cull_words.data[record + VELK_RECORD_ENTRY];
    if (entry >= VELK_CUT_RAY_QUAD || velk_cut_for_rays(entry)) return true;
    uint slot = cull_words.data[record + VELK_RECORD_SLOT];
    uint instance = pc.instances_base + slot;
    if (pc.items_word != 0xFFFFFFFFu) instance = cull_words.data[pc.items_word + slot * VELK_PLAIN_ITEM_WORDS];
    if (instance == 0xFFFFFFFFu) return false;
    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)), dot(world[2].xyz, world[2].xyz)));
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cull_words.data[record + VELK_RECORD_CLUSTER] * VELK_LOD_CLUSTER_WORDS;
    return velk_ray_pixels(cluster + VELK_LOD_CLUSTER_PARENT_SPHERE, world, scale, viewpoint, pixels_per_unit) > pc.light_pixels;
}

// Where the partition at state word `state` chose from, and how many pixels a unit is at a unit
// away in the view, which velk_ray_record goes by.
vec3 velk_ray_viewpoint(uint state)
{
    return vec3(uintBitsToFloat(cull_words.data[state + VELK_PART_FROM]), uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 1u]),
                uintBitsToFloat(cull_words.data[state + VELK_PART_FROM + 2u]));
}

float velk_ray_pixels_per_unit()
{
    GlobalData g = velk_globals.data[pc.globals_base];
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
}
