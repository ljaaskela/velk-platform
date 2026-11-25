// What of the lights `lights` the pixel at `coord` gets, `through`, from the soft shadows whose
// words start at `base` (VELK_SHADOW_SOFT_GLSL): each light the softening spread over its
// penumbra takes what it found, the rest keep what the points of light say. For the lighting,
// whose velk_light_cell_words is the culling arena.
void velk_shadow_apply(uint base, ivec2 coord, uint lights[VELK_LIGHT_SLOTS], inout float through[VELK_LIGHT_SLOTS])
{
    uint width = velk_light_cell_words.data[base + VELK_SHADOW_WIDTH];
    uint height = velk_light_cell_words.data[base + VELK_SHADOW_HEIGHT];
    if (uint(coord.x) >= width || uint(coord.y) >= height) return;
    uint pixels = width * height;
    uint pixel = uint(coord.y) * width + uint(coord.x);
    uint soft = velk_light_cell_words.data[velk_shadow_soft_word(base, pixels, pixel)];
    if (soft == 0xFFFFFFFFu) return;
    uint at = velk_shadow_pack_word(base, pixel);
    for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
        uint v = (soft >> (8u * j)) & 255u;
        uint packed = velk_light_cell_words.data[at + j];
        if (v == VELK_SHADOW_KEEP || packed == 0xFFFFFFFFu) continue;
        uint light = velk_shadow_light(packed);
        for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
            if (lights[k] == light) through[k] = float(v) / 254.0;
        }
    }
}
