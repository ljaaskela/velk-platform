layout(push_constant, scalar) uniform VelkPC {
    uint draw_base;
    uint pool_word;          // the light pool, VELK_LIGHT_NONE for none
    float light_pixels;      // kLightPixels
    uint cells_header;       // word of the light header. See CELLS_HEADER
    uint light_count;
    uint lights_base;
    uint env_texture_id;
    vec2 env_params;         // x = intensity, y = rotation_rad
    uint ltc_matrix_id;
    uint ltc_magnitude_id;
} pc;

#define VELK_GLOBALS velk_globals.data[velk_draw_data.data[pc.draw_base].globals_base]
layout(set = 1, binding = 16, std430) readonly buffer VelkLightCellWords { uint data[]; } velk_light_cell_words;
#define CELLS_HEADER pc.cells_header
#define VELK_LIGHT_WORDS velk_light_cell_words
