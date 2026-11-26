// The soft shadows' first pass, a thread a pixel, a tile a group: of the lights the points of
// light give the pixel (velk_light_direct), up to VELK_SHADOW_LIGHTS that have a size, those that
// bring it the most first, each with what of it gets through and how far away what keeps it is
// (the nearest blocker code of the points the pixel reads); and for the tile, whether one is kept
// from some of it and reaches some. The softened word starts as none. The first thread also
// empties the list of tiles to soften. pc.spare0 is the soft shadows' first word, pc.spare3 the
// pool, pc.spare4 and pc.spare2 the pixel ids' word and width, pc.spare5 kLightPixels.

// How much light `l` brings a surface at `p` facing `N`, as a luminance: a stand-in (`stand_in`
// 0 or more) whichever way it faces, a thin sheet passing `transmission` of its light from both.
float velk_shadow_weight(Light l, vec3 p, vec3 N, float stand_in, float transmission)
{
    vec3 L = l.flags.x == 0u ? -l.direction.xyz : l.position.xyz - p;
    L = L / max(length(L), 1e-6);
    float d = dot(N, L);
    float facing = stand_in >= 0.0 ? 1.0 : mix(max(d, 0.0), max(-d, 0.0), transmission);
    return velk_light_strength(l, p) * facing;
}

// Whether light `l` has a size, so that its shadows have penumbrae.
bool velk_shadow_sized(Light l)
{
    return l.flags.x <= 3u && l.params.w > 0.0;
}

shared uint s_tile_flags;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint base = pc.spare0;
    uint width = velk_light_cell_words.data[base + VELK_SHADOW_WIDTH];
    uint height = velk_light_cell_words.data[base + VELK_SHADOW_HEIGHT];
    uint pixels = width * height;
    if (gl_LocalInvocationIndex == 0u) s_tile_flags = 0u;
    if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) {
        velk_light_cell_words.data[base + VELK_SHADOW_ARGS] = 0u;
        velk_light_cell_words.data[base + VELK_SHADOW_ARGS + 1u] = 1u;
        velk_light_cell_words.data[base + VELK_SHADOW_ARGS + 2u] = 1u;
        velk_light_cell_words.data[base + VELK_SHADOW_LISTED] = 0u;
    }
    barrier();

    uint flags = 0u;
    if (uint(coord.x) < width && uint(coord.y) < height) {
        uint pixel = uint(coord.y) * width + uint(coord.x);
        uint packed[VELK_SHADOW_LIGHTS];
        float most[VELK_SHADOW_LIGHTS];
        for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
            packed[j] = VELK_LIGHT_NONE;
            most[j] = 0.0;
        }
        vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
        vec3 world_n = velk_texture(pc.normal_tex_id, uv).xyz;
        vec4 position = velk_texture(pc.worldpos_tex_id, uv);
        vec4 mat = velk_texture(pc.material_tex_id, uv);
        uint lighting_mode = uint(mat.b * 255.0 + 0.5);
        if (dot(world_n, world_n) >= 1e-6 && lighting_mode != 0u && pc.spare3 != VELK_LIGHT_NONE) {
            vec3 N = normalize(world_n);
            float transmission = velk_texture(pc.emissive_tex_id, uv).a;
            float stand_in = velk_stand_in_unpack(mat.a);
            uint lights[VELK_LIGHT_SLOTS];
            float through[VELK_LIGHT_SLOTS];
            uint radiance;
            uint radiance_dir;
            g_light_codes_first = VELK_LIGHT_NONE;
            g_light_slot_codes = 0xFFFFFFFFu;
            bool points = velk_light_direct(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord),
                                            velk_pixel_weights(pc.spare4, pc.spare2, coord), position.xyz, position.w,
                                            uintBitsToFloat(pc.spare5), lights, through, radiance, radiance_dir);
            for (uint k = 0u; k < VELK_LIGHT_SLOTS && points; ++k) {
                uint i = lights[k];
                if (i == VELK_LIGHT_NONE || i >= pc.light_count || i > 0xFFFEu) continue;
                Light l = velk_light_at(pc.lights_base + i);
                if (!velk_shadow_sized(l)) continue;
                float w = velk_shadow_weight(l, position.xyz, N, stand_in, transmission);
                if (w <= 0.0) continue;
                // How far away what keeps it is, from the points the pixel read: 0 for not known.
                uint code = 0u;
                uint s = velk_light_codes_slot(i);
                if (s < VELK_LIGHT_SLOTS) {
                    uint c = (g_light_slot_codes >> (4u * s)) & 15u;
                    if (velk_light_code_blocked(c)) code = c;
                }
                uint p = velk_shadow_pack(i, through[k], code);
                // In order, those bringing the most first; of two alike, the lower light first.
                for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
                    if (w > most[j] || (w == most[j] && packed[j] != VELK_LIGHT_NONE && velk_shadow_light(p) < velk_shadow_light(packed[j]))) {
                        float wm = most[j];
                        uint pm = packed[j];
                        most[j] = w;
                        packed[j] = p;
                        w = wm;
                        p = pm;
                    }
                }
            }
            for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
                if (packed[j] == VELK_LIGHT_NONE) continue;
                float t = velk_shadow_through(packed[j]);
                if (t < 1.0) flags |= VELK_SHADOW_TILE_SHADOW;
                if (t > 0.0) flags |= VELK_SHADOW_TILE_LIT;
            }
        }
        uint at = velk_shadow_pack_word(base, pixel);
        for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) velk_light_cell_words.data[at + j] = packed[j];
        velk_light_cell_words.data[velk_shadow_soft_word(base, pixels, pixel)] = 0xFFFFFFFFu;
    }
    if (flags != 0u) atomicOr(s_tile_flags, flags);
    barrier();
    uint tiles_x = (width + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE;
    uint tiles_y = (height + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE;
    if (gl_LocalInvocationIndex == 0u && gl_WorkGroupID.x < tiles_x && gl_WorkGroupID.y < tiles_y) {
        velk_light_cell_words.data[velk_shadow_tile_word(base, pixels, gl_WorkGroupID.y * tiles_x + gl_WorkGroupID.x)] = s_tile_flags;
    }
}
