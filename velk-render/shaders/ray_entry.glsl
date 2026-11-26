void velk_store_box(uint at, vec3 lo, vec3 hi)
{
    cull_words.data[at] = floatBitsToUint(lo.x);
    cull_words.data[at + 1u] = floatBitsToUint(lo.y);
    cull_words.data[at + 2u] = floatBitsToUint(lo.z);
    cull_words.data[at + 3u] = floatBitsToUint(hi.x);
    cull_words.data[at + 4u] = floatBitsToUint(hi.y);
    cull_words.data[at + 5u] = floatBitsToUint(hi.z);
}

vec3 velk_load_vec3(uint at)
{
    return vec3(uintBitsToFloat(cull_words.data[at]), uintBitsToFloat(cull_words.data[at + 1u]),
                uintBitsToFloat(cull_words.data[at + 2u]));
}

// Writes the ray list entry at word `at` for the record at pool place `place`: the place and
// the record's box, grown by the pad.
void velk_write_ray_entry(uint at, uint place)
{
    vec3 rlo, rhi;
    velk_cut_record_box(velk_cut_record_word(pc.cut_word, place), pc.instances_base, pc.items_word, pc.pad, rlo, rhi);
    cull_words.data[at + VELK_CUT_RAY_INDEX] = place;
    velk_store_box(at + VELK_CUT_RAY_MIN, rlo, rhi);
}

// The box of the ray list entry at word `at`, and its centre and the radius of the sphere round
// it, which the sorts go by; false for a record without a box.
bool velk_ray_entry_box(uint at, out vec3 lo, out vec3 hi, out vec3 centre, out float radius)
{
    lo = velk_load_vec3(at + VELK_CUT_RAY_MIN);
    hi = velk_load_vec3(at + VELK_CUT_RAY_MAX);
    centre = (lo + hi) * 0.5;
    radius = length(hi - lo) * 0.5;
    return lo.x <= hi.x;
}

// The sort's code of a list past kSortMax entries for the entry at word `at`, its list's centres
// within `lo` to `hi`, `extent` its widest side: all 30 bits of its Morton code under the bit for
// the wide ones, and above every other for one without a box.
uint velk_ray_entry_code(uint at, vec3 lo, vec3 hi, float extent)
{
    vec3 blo, bhi, c;
    float radius;
    if (!velk_ray_entry_box(at, blo, bhi, c, radius)) return 0xFFFFFFFFu;
    return ((radius >= 0.25 * extent ? 1u : 0u) << 30u) | velk_morton(c, lo, hi);
}
