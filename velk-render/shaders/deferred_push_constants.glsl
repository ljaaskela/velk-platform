// scalar layout: tight packing so the per-dispatch CPU struct (whose
// `vec4 cam_pos` sits at offset 0 of the struct) lines up with the
// GPU's view of cam_pos at offset 8 of the push-constant block. Default
// std430 would round vec4 up to a 16-byte alignment and shift every
// subsequent field by 8 bytes vs. what the CPU writes.
layout(push_constant, scalar) uniform PC {
    uint globals_base;         // offset 0  (4 bytes; view's FrameGlobals index)
    uint spare0;               // 4
    vec4 cam_pos;              // 8         (CPU push starts here)
    uint output_image_id;      // 24
    uint albedo_tex_id;        // 28
    uint normal_tex_id;        // 32
    uint worldpos_tex_id;      // 36
    uint material_tex_id;      // 40
    uint emissive_tex_id;      // 44
    uint width;                // 48
    uint height;               // 52
    uint light_count;          // 56
    uint env_texture_id;       // 60
    uint cells_header;         // 64  word of the light header. See CELLS_HEADER
    uint ltc_matrix_id;        // 68  LTC transform table; 0 = area specular off
    uint lights_base;          // 72  index into velk_lights (set = 1 slot 5)
    uint ltc_magnitude_id;     // 76  LTC energy / Fresnel table
    vec2 env_params;           // 80  x = intensity, y = rotation_rad (inline)
    uint spare6;               // 88
    uint spare1;               // 92
    uint mode;                 // 96  what a pass is to do, for passes that do more than one thing
    uint spare3;               // 100
    uint spare4;               // 104
    uint spare2;               // 108
    uint spare5;               // 112
} pc;
